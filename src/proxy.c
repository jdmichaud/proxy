/* SPDX-License-Identifier: MIT */
/*
 * Sample program that can act either as a packet sink, where it just receives
 * packets and doesn't do anything with them, or it can act as a proxy where it
 * receives packets and then sends them to a new destination.
 * 
 * Examples:
 *
 * Act as a proxy, listening on port 4444, and send data to 192.168.2.6 on port
 * 4445. Use multishot receive, DEFER_TASKRUN, and fixed files
 *
 * 	./proxy -m1 -d1 -f1 -H 192.168.2.6 -r4444 -p4445
 *
 * Act as a bi-directional proxy, listening on port 8884, and send data back
 * and forth between host and 192.168.2.6 on port 22. Use multishot receive,
 * DEFER_TASKRUN, fixed files, and buffers of size 1500.
 *
 * 	./proxy -m1 -d1 -f1 -B1 -b1500 -H 192.168.2.6 -r22 -p8888
 *
 * Act a sink, listening on port 4445, using multishot receive, DEFER_TASKRUN,
 * and fixed files:
 *
 * 	./proxy -m1 -d1 -s1 -f1 -p4445
 *
 * Run with -h to see a list of options, and their defaults.
 *
 * (C) 2024 Jens Axboe <axboe@kernel.dk>
 *
 * Modified by Jean-Daniel Michaud to deal with "localhost" as parameters
 * (line 825-844).
 *
 * Compile with:
 * cc -Wall -fsanitize=address -fsanitize=leak -fsanitize=undefined proxy.c -o proxy -luring
 *
 * Dependencies:
 * - list.h (from https://git.kernel.dk/cgit/liburing/tree/examples/)
 * - liburing (on debian: apt install liburing-dev)
 */
#include <fcntl.h>
#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <liburing.h>

#include "list.h"

/*
 * Goes from accept new connection -> create socket, connect to end
 * point, prepare recv, on receive do send (unless sink). If either ends
 * disconnects, we transition to shutdown and then close.
 */
enum {
	__ACCEPT	= 0,
	__SOCK		= 1,
	__CONNECT	= 2,
	__RECV		= 3,
	__SEND		= 4,
	__SHUTDOWN	= 5,
	__CLOSE		= 6,
};

/*
 * Generic opcode agnostic encoding to sqe/cqe->user_data
 */
struct userdata {
	union {
		struct {
			uint16_t op_tid; /* 3 bits op, 13 bits tid */
			uint16_t bgid;
			uint16_t bid;
			uint16_t fd;
		};
		uint64_t val;
	};
};

#define OP_SHIFT	(13)
#define TID_MASK	((1U << 13) - 1)

static int start_bgid = 1;

#define MAX_CONNS	1024

static int nr_conns;
static int mshot = 1;
static int sqpoll;
static int defer_tw = 1;
static int is_sink;
static int fixed_files = 1;
static char *host = "192.168.2.6";
static int send_port = 4445;
static int receive_port = 4444;
static int buf_size = 32;
static int bidi;
static int ipv6;
static int verbose;

static int nr_bufs = 256;
static int br_mask;

#define NR_BUF_RINGS	2

struct conn_buf_ring {
	struct io_uring_buf_ring *br;
	void *buf;
	int bgid;
};

struct pending_send {
	struct list_head list;

	int fd, bgid, bid, len;
	void *data;
};

/*
 * Per socket stats per connection. For bi-directional, we'll have both
 * sends and receives on each socket, this helps track them seperately.
 * For sink or one directional, each of the two stats will be only sends
 * or receives, not both.
 */
struct conn_dir {
	int pending_shutdown;
	int pending_sends;
	struct list_head send_list;

	int rcv, rcv_shrt;
	int snd, snd_shrt;
	int snd_busy;

	unsigned long in_bytes, out_bytes;

	int bgid_switch;
	int mshot_resubmit;
};

enum {
	CONN_F_DISCONNECTING	= 1,
	CONN_F_DISCONNECTED	= 2,
	CONN_F_STATS_SHOWN	= 4,
};

struct conn {
	struct conn_buf_ring brs[NR_BUF_RINGS];
	struct conn_buf_ring *cur_br;

	int tid;
	int in_fd, out_fd;
	int start_bgid;
	int cur_br_index;
	int flags;

	unsigned long rps;

	struct conn_dir cd[2];

	union {
		struct sockaddr_in addr;
		struct sockaddr_in6 addr6;
	};
};

static struct conn conns[MAX_CONNS];

static int setup_listening_socket(int port)
{
	struct sockaddr_in srv_addr = { };
	struct sockaddr_in6 srv_addr6 = { };
	int fd, enable, ret, domain;

	if (ipv6)
		domain = AF_INET6;
	else
		domain = AF_INET;

	fd = socket(domain, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket()");
		return -1;
	}

	enable = 1;
	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
	if (ret < 0) {
		perror("setsockopt(SO_REUSEADDR)");
		return -1;
	}

	if (ipv6) {
		srv_addr6.sin6_family = AF_INET6;
		srv_addr6.sin6_port = htons(port);
		srv_addr6.sin6_addr = in6addr_any;
		ret = bind(fd, (const struct sockaddr *)&srv_addr6, sizeof(srv_addr6));
	} else {
		srv_addr.sin_family = AF_INET;
		srv_addr.sin_port = htons(port);
		srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		ret = bind(fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr));
	}

	if (ret < 0) {
		perror("bind()");
		return -1;
	}

	if (listen(fd, 1024) < 0) {
		perror("listen()");
		return -1;
	}

	return fd;
}

/*
 * Setup 2 ring provided buffer rings for each connection. If we get -ENOBUFS
 * on receive, we'll switch to the other ring and re-arm. If this happens
 * frequently (see switch= stat), then the ring sizes are likely too small.
 * Use -nXX to make them bigger.
 *
 * The alternative here would be to use the older style provided buffers,
 * where you simply setup a buffer group and use SQEs with
 * io_urign_prep_provide_buffers() to add to the pool. But that approach is
 * slower and has been deprecated by using the faster ring provided buffers.
 */
static int setup_buffer_ring(struct io_uring *ring, struct conn *c, int index)
{
	struct conn_buf_ring *cbr = &c->brs[index];
	int ret, i;
	void *ptr;

	cbr->bgid = c->start_bgid + index;

	if (posix_memalign(&cbr->buf, 4096, buf_size * nr_bufs)) {
		perror("posix memalign");
		return 1;
	}

	cbr->br = io_uring_setup_buf_ring(ring, nr_bufs, cbr->bgid, 0, &ret);
	if (!cbr->br) {
		fprintf(stderr, "Buffer ring register failed %d\n", ret);
		return 1;
	}

	ptr = cbr->buf;
	for (i = 0; i < nr_bufs; i++) {
		io_uring_buf_ring_add(cbr->br, ptr, buf_size, i, br_mask, i);
		ptr += buf_size;
	}
	io_uring_buf_ring_advance(cbr->br, nr_bufs);
	printf("%d: buffer ring bgid %d, bufs %d\n", c->tid, cbr->bgid, nr_bufs);
	return 0;
}

/*
 * Sets up two buffer rings per connection, and we alternate between them if we
 * hit -ENOBUFS on a receive. See handle_enobufs().
 */
static int setup_buffer_rings(struct io_uring *ring, struct conn *c)
{
	int i;

	c->start_bgid = start_bgid;

	for (i = 0; i < NR_BUF_RINGS; i++) {
		if (setup_buffer_ring(ring, c, i))
			return 1;
	}

	c->cur_br = &c->brs[0];
	c->cur_br_index = 0;
	start_bgid += 2;
	return 0;
}

static void free_buffer_rings(struct io_uring *ring, struct conn *c)
{
	int i;

	for (i = 0; i < NR_BUF_RINGS; i++) {
		struct conn_buf_ring *cbr = &c->brs[i];

		io_uring_free_buf_ring(ring, cbr->br, nr_bufs, cbr->bgid);
		free(cbr->buf);
	}

	c->cur_br = NULL;
}

static void __show_stats(struct conn *c)
{
	struct conn_dir *cd;
	int i;

	if (c->flags & CONN_F_STATS_SHOWN)
		return;

	printf("Conn %d/(in_fd=%d, out_fd=%d): rps=%lu\n", c->tid, c->in_fd,
							c->out_fd, c->rps);

	for (i = 0; i < 2; i++) {
		cd = &c->cd[i];

		if (!cd->in_bytes && !cd->out_bytes)
			continue;

		printf("\t%3d: rcv=%u (short=%u), snd=%u (short=%u, busy=%u)\n",
			i, cd->rcv, cd->rcv_shrt, cd->snd, cd->snd_shrt,
			cd->snd_busy);
		printf("\t   : switch=%u, mshot_resubmit=%d\n",
			cd->bgid_switch, cd->mshot_resubmit);
		printf("\t   : in_bytes=%lu (Kb %lu), out_bytes=%lu (Kb %lu)\n",
			cd->in_bytes, cd->in_bytes >> 10,
			cd->out_bytes, cd->out_bytes >> 10);
	}

	c->flags |= CONN_F_STATS_SHOWN;
}

static void show_stats(void)
{
	int i;

	for (i = 0; i < MAX_CONNS; i++) {
		struct conn *c = &conns[i];

		if (!c->rps)
			continue;

		__show_stats(c);
	}
}

static void sig_int(int __attribute__((__unused__)) sig)
{
	show_stats();
	exit(1);
}

/*
 * Special cased for SQPOLL only, as we don't control when SQEs are consumed if
 * that is used. Hence we may need to wait for the SQPOLL thread to keep up
 * until we can get a new SQE. All other cases will break immediately, with a
 * fresh SQE.
 *
 * If we grossly undersized our SQ ring, getting a NULL sqe can happen even
 * for the !SQPOLL case if we're handling a lot of CQEs in our event loop
 * and multishot isn't used. We can do io_uring_submit() to flush what we
 * have here. Only caveat here is that if linked requests are used, SQEs
 * would need to be allocated upfront as a link chain is only valid within
 * a single submission cycle.
 */
static struct io_uring_sqe *get_sqe(struct io_uring *ring)
{
	struct io_uring_sqe *sqe;

	do {
		sqe = io_uring_get_sqe(ring);
		if (sqe)
			break;
		if (!sqpoll)
			io_uring_submit(ring);
		else
			io_uring_sqring_wait(ring);
	} while (1);

	return sqe;
}

/*
 * Packs the information that we will need at completion time into the
 * sqe->user_data field, which is passed back in the completion in
 * cqe->user_data. Some apps would need more space than this, and in fact
 * I'd love to pack the requested IO size in here, and it's not uncommon to
 * see apps use this field as just a cookie to either index a data structure
 * at completion time, or even just put the pointer to the associated
 * structure into this field.
 */
static void __encode_userdata(struct io_uring_sqe *sqe, int tid, int op,
			    int bgid, int bid, int fd)
{
	struct userdata ud = {
		.op_tid = (op << OP_SHIFT) | tid,
		.bgid = bgid,
		.bid = bid,
		.fd = fd
	};

	io_uring_sqe_set_data64(sqe, ud.val);
}

static void encode_userdata(struct io_uring_sqe *sqe, struct conn *c, int op,
			    int bgid, int bid, int fd)
{
	__encode_userdata(sqe, c->tid, op, bgid, bid, fd);
}

static int cqe_to_op(struct io_uring_cqe *cqe)
{
	struct userdata ud = { .val = cqe->user_data };

	return ud.op_tid >> OP_SHIFT;
}

static struct conn *cqe_to_conn(struct io_uring_cqe *cqe)
{
	struct userdata ud = { .val = cqe->user_data };

	return &conns[ud.op_tid & TID_MASK];
}

static int cqe_to_bgid(struct io_uring_cqe *cqe)
{
	struct userdata ud = { .val = cqe->user_data };

	return ud.bgid;
}

static int cqe_to_bid(struct io_uring_cqe *cqe)
{
	struct userdata ud = { .val = cqe->user_data };

	return ud.bid;
}

static int cqe_to_fd(struct io_uring_cqe *cqe)
{
	struct userdata ud = { .val = cqe->user_data };

	return ud.fd;
}

static struct conn_dir *fd_to_conn_dir(struct conn *c, int fd)
{
	return &c->cd[fd != c->in_fd];
}

static void __submit_receive(struct io_uring *ring, struct conn *c, int fd)
{
	struct conn_buf_ring *cbr = c->cur_br;
	struct io_uring_sqe *sqe;

	if (verbose)
		printf("%d: submit receive fd=%d\n", c->tid, fd);

	/*
	 * For both recv and multishot receive, we use the ring provided
	 * buffers. These are handed to the application ahead of time, and
	 * are consumed when a receive triggers. Note that the address and
	 * length of the receive are set to NULL/0, and we assign the
	 * sqe->buf_group to tell the kernel which buffer group ID to pick
	 * a buffer from. Finally, IOSQE_BUFFER_SELECT is set to tell the
	 * kernel that we want a buffer picked for this request, we are not
	 * passing one in with the request.
	 */
	sqe = get_sqe(ring);
	if (mshot)
		io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
	else
		io_uring_prep_recv(sqe, fd, NULL, 0, 0);

	encode_userdata(sqe, c, __RECV, cbr->bgid, 0, fd);
	sqe->buf_group = cbr->bgid;
	sqe->flags |= IOSQE_BUFFER_SELECT;
	if (fixed_files)
		sqe->flags |= IOSQE_FIXED_FILE;
}

/*
 * One directional just arms receive on our in_fd
 */
static void submit_receive(struct io_uring *ring, struct conn *c)
{
	__submit_receive(ring, c, c->in_fd);
}

/*
 * Bi-directional arms receive on both in and out fd
 */
static void submit_bidi_receive(struct io_uring *ring, struct conn *c)
{
	__submit_receive(ring, c, c->in_fd);
	__submit_receive(ring, c, c->out_fd);
}

/*
 * We hit -ENOBUFS, which means that we ran out of buffers in our current
 * provided buffer group. This can happen if there's an imbalance between the
 * receives coming in and the sends being processed. Switch to the other buffer
 * group and continue from there, previous sends should come in and replenish the
 * previous one by the time we potentially hit -ENOBUFS again.
 */
static void handle_enobufs(struct io_uring *ring, struct conn *c,
			   struct conn_dir *cd, int fd)
{
	cd->bgid_switch++;
	c->cur_br_index ^= 1;
	c->cur_br = &c->brs[c->cur_br_index];

	if (verbose) {
		printf("%d: enobufs: switch to bgid %d\n", c->tid,
							c->cur_br->bgid);
	}

	__submit_receive(ring, c, fd);
}

/*
 * Kill this socket - submit a shutdown and link a close to it. We don't
 * care about shutdown status, so mark it as not needing to post a CQE unless
 * it fails.
 */
static void queue_shutdown_close(struct io_uring *ring, struct conn *c, int fd)
{
	struct io_uring_sqe *sqe1, *sqe2;

	/*
	 * On the off chance that we run out of SQEs after the first one,
	 * grab two upfront. This it to prevent our link not working if
	 * get_sqe() ends up doing submissions to free up an SQE, as links
	 * are not valid across separate submissions.
	 */
	sqe1 = get_sqe(ring);
	sqe2 = get_sqe(ring);

	io_uring_prep_shutdown(sqe1, fd, SHUT_RDWR);
	if (fixed_files)
		sqe1->flags |= IOSQE_FIXED_FILE;
	sqe1->flags |= IOSQE_IO_LINK | IOSQE_CQE_SKIP_SUCCESS;
	encode_userdata(sqe1, c, __SHUTDOWN, 0, 0, fd);

	if (fixed_files)
		io_uring_prep_close_direct(sqe2, fd);
	else
		io_uring_prep_close(sqe2, fd);
	encode_userdata(sqe2, c, __CLOSE, 0, 0, fd);
}

static int pending_shutdown(struct conn *c)
{
	return c->cd[0].pending_shutdown + c->cd[1].pending_shutdown;
}

static void __close_conn(struct io_uring *ring, struct conn *c)
{
	printf("Client %d: queueing shutdown\n", c->tid);

	queue_shutdown_close(ring, c, c->in_fd);
	queue_shutdown_close(ring, c, c->out_fd);
	io_uring_submit(ring);
}

static void close_cd(struct conn_dir *cd)
{
	if (cd->pending_sends)
		return;

	cd->pending_shutdown = 1;
}

static void __queue_send(struct io_uring *ring, struct conn *c, int fd,
			 void *data, int len, int bgid, int bid)
{
	struct conn_dir *cd = fd_to_conn_dir(c, fd);
	struct io_uring_sqe *sqe;

	if (verbose) {
		printf("%d: send %d to fd %d (%p, bgid %d, bid %d)\n", c->tid,
				len, fd, data, bgid, bid);
	}

	sqe = get_sqe(ring);
	io_uring_prep_send(sqe, fd, data, len, MSG_WAITALL);
	encode_userdata(sqe, c, __SEND, bgid, bid, fd);
	if (fixed_files)
		sqe->flags |= IOSQE_FIXED_FILE;
	cd->pending_sends++;
}

/*
 * Submit any deferred sends (see comment for defer_send()).
 */
static void submit_deferred_send(struct io_uring *ring, struct conn *c,
				 struct conn_dir *cd)
{
	struct pending_send *ps;

	if (list_empty(&cd->send_list)) {
		if (verbose)
			printf("%d: defer send %p empty\n", c->tid, cd);
		return;
	}

	if (verbose)
		printf("%d: queueing deferred send %p\n", c->tid, cd);

	ps = list_first_entry(&cd->send_list, struct pending_send, list);
	list_del(&ps->list);
	__queue_send(ring, c, ps->fd, ps->data, ps->len, ps->bgid, ps->bid);
	free(ps);
}

/*
 * We have pending sends on this socket. Normally this is not an issue, but
 * if we don't serialize sends, then we can get into a situation where the
 * following can happen:
 *
 * 1) Submit sendA for socket1
 * 2) socket1 buffer is full, poll is armed for sendA
 * 3) socket1 space frees up
 * 4) Poll triggers retry for sendA
 * 5) Submit sendB for socket1
 * 6) sendB completes
 * 7) sendA is retried
 *
 * Regardless of the outcome of what happens with sendA in step 7 (it completes
 * or it gets deferred because the socket1 buffer is now full again after sendB
 * has been filled), we've now reordered the received data.
 *
 * This isn't a common occurence, but more likely with big buffers. If we never
 * run into out-of-space in the socket, we could easily support having more than
 * one send in-flight at the same time.
 *
 * Something to think about on the kernel side...
 */
static void defer_send(struct conn *c, struct conn_dir *cd, void *data,
		       int len, int bgid, int bid, int out_fd)
{
	struct pending_send *ps = malloc(sizeof(*ps));

	if (verbose) {
		printf("%d: defer send %d to fd %d (%p, bgid %d, bid %d)\n",
			c->tid, len, out_fd, data, bgid, bid);
		printf("%d: pending %d, %p\n", c->tid, cd->pending_sends, cd);
	}

	cd->snd_busy++;
	ps->fd = out_fd;
	ps->bgid = bgid;
	ps->bid = bid;
	ps->len = len;
	ps->data = data;
	list_add_tail(&ps->list, &cd->send_list);
}

static void queue_send(struct io_uring *ring, struct conn *c, void *data,
		       int len, int bgid, int bid, int out_fd)
{
	struct conn_dir *cd = fd_to_conn_dir(c, out_fd);

	if (cd->pending_sends)
		defer_send(c, cd, data, len, bgid, bid, out_fd);
	else
		__queue_send(ring, c, out_fd, data, len, bgid, bid);
}

static int handle_receive(struct io_uring *ring, struct conn *c,
			  struct io_uring_cqe *cqe, int in_fd, int out_fd)
{
	struct conn_dir *cd = fd_to_conn_dir(c, in_fd);
	struct conn_buf_ring *cbr;
	int bid, bgid, do_recv = !mshot;
	int res = cqe->res;
	void *ptr;

	if (res < 0) {
		if (res == -ENOBUFS) {
			handle_enobufs(ring, c, cd, in_fd);
			return 0;
		} else {
			fprintf(stderr, "recv error %s\n", strerror(-res));
			return 1;
		}
	}

	if (res != buf_size)
		cd->rcv_shrt++;

	if (!(cqe->flags & IORING_CQE_F_BUFFER)) {
		if (!res) {
			close_cd(cd);
			return 0;
		}
		fprintf(stderr, "no buffer assigned, res=%d\n", res);
		return 1;
	}

	cd->rcv++;

	/*
	 * If multishot terminates, just submit a new one.
	 */
	if (mshot && !(cqe->flags & IORING_CQE_F_MORE)) {
		cd->mshot_resubmit++;
		do_recv = 1;
	}

	bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
	bgid = cqe_to_bgid(cqe);

	if (verbose) {
		printf("%d: recv: bid=%d, bgid=%d, res=%d\n", c->tid, bid, bgid,
								res);
	}

	cbr = &c->brs[bgid - c->start_bgid];
	ptr = cbr->buf + bid * buf_size;

	/*
	 * If we're a sink, we're done here. Just replenish the buffer back
	 * to the pool. For proxy mode, we will send the data to the other
	 * end and the buffer will be replenished once the send is done with
	 * it.
	 */
	if (is_sink) {
		io_uring_buf_ring_add(cbr->br, ptr, buf_size, bid, br_mask, 0);
		io_uring_buf_ring_advance(cbr->br, 1);
	} else {
		queue_send(ring, c, ptr, res, bgid, bid, out_fd);
	}

	c->rps++;
	cd->in_bytes += res;

	/*
	 * If we're not doing multishot receive, or if multishot receive
	 * terminated, we need to submit a new receive request as this one
	 * has completed. Multishot will stay armed.
	 */
	if (do_recv)
		__submit_receive(ring, c, in_fd);

	return 0;
}

static int handle_accept(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct io_uring_sqe *sqe;
	struct conn *c;
	int domain;

	if (cqe->res < 0) {
		fprintf(stderr, "accept error %s\n", strerror(-cqe->res));
		return 1;
	}

	if (nr_conns == MAX_CONNS) {
		fprintf(stderr, "max clients reached %d\n", nr_conns);
		return 1;
	}

	c = &conns[nr_conns];
	c->tid = nr_conns;
	c->in_fd = cqe->res;

	printf("New client: id=%d, in=%d\n", nr_conns, c->in_fd);

	nr_conns++;
	setup_buffer_rings(ring, c);
	init_list_head(&c->cd[0].send_list);
	init_list_head(&c->cd[1].send_list);

	if (is_sink) {
		submit_receive(ring, c);
		return 0;
	}

	if (ipv6)
		domain = AF_INET6;
	else
		domain = AF_INET;

	/*
	 * If fixed_files is set, proxy will use fixed files for any
	 * new file descriptors it instantiates. Fixd files, or fixed
	 * descriptors, are io_uring private file descriptors. They
	 * cannot be accessed outside of io_uring. io_uring holds a
	 * fixed reference to them, which means that we do not need to
	 * grab per-request references to them. Particularly for
	 * threaded applications, grabbing and dropping file references
	 * for each operation can be costly as the file table is shared.
	 * This generally shows up as fget/fput related overhead in
	 * any workload profiles.
	 *
	 * Fixed descriptors are passed in via the 'fd' field just
	 * like regular descriptors, and then marked as such by
	 * setting the IOSQE_FIXED_FILE flag in the sqe->flags field.
	 * Some helpers do that automatically, like the below, others
	 * will need it set manually if they don't have a *direct*()
	 * helper.
	 *
	 * For operations that instantiate them, like the opening of
	 * a direct socket, the application may either ask the kernel
	 * to find a free one (as is done below), or the application
	 * may manage the space itself and pass in an index for a
	 * currently free slot in the table. If the kernel is asked
	 * to allocate a free direct descriptor, note that io_uring
	 * does not abide by the POSIX mandated "lowest free must be
	 * returned". It may return any free descriptor of its
	 * choosing.
	 */
	sqe = get_sqe(ring);
	if (fixed_files)
		io_uring_prep_socket_direct_alloc(sqe, domain, SOCK_STREAM, 0, 0);
	else
		io_uring_prep_socket(sqe, domain, SOCK_STREAM, 0, 0);
	encode_userdata(sqe, c, __SOCK, 0, 0, 0);
	return 0;
}

static int handle_sock(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct conn *c = cqe_to_conn(cqe);
	struct io_uring_sqe *sqe;
	int ret;

	if (cqe->res < 0) {
		fprintf(stderr, "socket error %s\n", strerror(-cqe->res));
		return 1;
	}

	if (verbose)
		printf("%d: sock: res=%d\n", c->tid, cqe->res);

	c->out_fd = cqe->res;

	if (ipv6) {
		memset(&c->addr6, 0, sizeof(c->addr6));
		c->addr6.sin6_family = AF_INET6;
		c->addr6.sin6_port = htons(send_port);
		ret = inet_pton(AF_INET6, host, &c->addr6.sin6_addr);
	} else {
		memset(&c->addr, 0, sizeof(c->addr));
		c->addr.sin_family = AF_INET;
		c->addr.sin_port = htons(send_port);
    struct hostent *he;
    if ((he = gethostbyname(host)) == NULL) {
			fprintf(stderr, "gethostbyname: host not in right format\n");
      perror("gethostbyname");
      return 1;
    }
    struct in_addr addr;
    memcpy(&addr, he->h_addr_list[0], sizeof(struct in_addr));
		ret = inet_pton(AF_INET, inet_ntoa(addr), &c->addr.sin_addr);
	}
	if (ret <= 0) {
		if (!ret)
			fprintf(stderr, "host not in right format\n");
		else
			perror("inet_pton");
		return 1;
	}

	sqe = get_sqe(ring);
	if (ipv6) {
		io_uring_prep_connect(sqe, c->out_fd,
					(struct sockaddr *) &c->addr6,
					sizeof(c->addr6));
	} else {
		io_uring_prep_connect(sqe, c->out_fd,
					(struct sockaddr *) &c->addr,
					sizeof(c->addr));
	}
	encode_userdata(sqe, c, __CONNECT, 0, 0, c->out_fd);
	if (fixed_files)
		sqe->flags |= IOSQE_FIXED_FILE;
	return 0;
}

static int handle_connect(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct conn *c = cqe_to_conn(cqe);

	if (cqe->res < 0) {
		fprintf(stderr, "connect error %s\n", strerror(-cqe->res));
		return 1;
	}

	if (bidi)
		submit_bidi_receive(ring, c);
	else
		submit_receive(ring, c);

	return 0;
}

static int handle_recv(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct conn *c = cqe_to_conn(cqe);
	int fd = cqe_to_fd(cqe);

	if (fd == c->in_fd)
		return handle_receive(ring, c, cqe, c->in_fd, c->out_fd);

	return handle_receive(ring, c, cqe, c->out_fd, c->in_fd);
}

static int handle_send(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct conn *c = cqe_to_conn(cqe);
	struct conn_buf_ring *cbr;
	int fd = cqe_to_fd(cqe);
	struct conn_dir *cd = fd_to_conn_dir(c, fd);
	int bid, bgid;
	void *ptr;

	if (cqe->res < 0) {
		fprintf(stderr, "send error %s\n", strerror(-cqe->res));
		return 1;
	}

	cd->snd++;
	cd->out_bytes += cqe->res;

	if (cqe->res != buf_size)
		cd->snd_shrt++;

	bid = cqe_to_bid(cqe);
	bgid = cqe_to_bgid(cqe);

	if (verbose)
		printf("%d: send: bid=%d, bgid=%d, res=%d\n", c->tid, bid, bgid, cqe->res);

	/*
	 * Find the provided buffer that the receive consumed, and
	 * which we then used for the send, and add it back to the
	 * pool so it can get picked by another receive. Once the send
	 * is done, we're done with it.
	 */
	bgid -= c->start_bgid;
	cbr = &c->brs[bgid];
	ptr = cbr->buf + bid * buf_size;

	io_uring_buf_ring_add(cbr->br, ptr, buf_size, bid, br_mask, 0);
	io_uring_buf_ring_advance(cbr->br, 1);

	cd->pending_sends--;

	if (verbose)
		printf("%d: pending sends %d\n", c->tid, cd->pending_sends);

	if (!cd->pending_sends) {
		if (!cqe->res)
			close_cd(cd);
		else
			submit_deferred_send(ring, c, cd);
	}

	return 0;
}

/*
 * We don't expect to get here, as we marked it with skipping posting a
 * CQE if it was successful. If it does trigger, than means it fails and
 * that our close has not been done. Log the shutdown error and issue a new
 * separate close.
 */
static int handle_shutdown(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct conn *c = cqe_to_conn(cqe);
	struct io_uring_sqe *sqe;
	int fd = cqe_to_fd(cqe);

	fprintf(stderr, "Got shutdown notication on fd %d\n", fd);

	if (!cqe->res)
		fprintf(stderr, "Unexpected success shutdown CQE\n");
	else if (cqe->res < 0)
		fprintf(stderr, "Shutdown got %s\n", strerror(-cqe->res));

	sqe = get_sqe(ring);
	if (fixed_files)
		io_uring_prep_close_direct(sqe, fd);
	else
		io_uring_prep_close(sqe, fd);
	encode_userdata(sqe, c, __CLOSE, 0, 0, fd);
	return 0;
}

/*
 * Final stage of a connection, the shutdown and close has finished. Mark
 * it as disconnected and let the main loop reap it.
 */
static int handle_close(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	struct conn *c = cqe_to_conn(cqe);
	int fd = cqe_to_fd(cqe);

	c->flags |= CONN_F_DISCONNECTED;

	printf("Closed client: id=%d, in_fd=%d, out_fd=%d\n", c->tid, c->in_fd, c->out_fd);
	if (fd == c->in_fd)
		c->in_fd = -1;
	else if (fd == c->out_fd)
		c->out_fd = -1;

	if (c->in_fd == -1 && c->out_fd == -1) {
		__show_stats(c);
		free_buffer_rings(ring, c);
	}

	return 0;
}

/*
 * Called for each CQE that we receive. Decode the request type that it
 * came from, and call the appropriate handler.
 */
static int handle_cqe(struct io_uring *ring, struct io_uring_cqe *cqe)
{
	int ret;

	switch (cqe_to_op(cqe)) {
	case __ACCEPT:
		ret = handle_accept(ring, cqe);
		break;
	case __SOCK:
		ret = handle_sock(ring, cqe);
		break;
	case __CONNECT:
		ret = handle_connect(ring, cqe);
		break;
	case __RECV:
		ret = handle_recv(ring, cqe);
		break;
	case __SEND:
		ret = handle_send(ring, cqe);
		break;
	case __SHUTDOWN:
		ret = handle_shutdown(ring, cqe);
		break;
	case __CLOSE:
		ret = handle_close(ring, cqe);
		break;
	default:
		fprintf(stderr, "bad user data %lx\n", (long) cqe->user_data);
		return 1;
	}

	return ret;
}

static void usage(const char *name)
{
	printf("%s:\n", name);
	printf("\t-m:\t\tUse multishot receive (%d)\n", mshot);
	printf("\t-d:\t\tUse DEFER_TASKRUN (%d)\n", defer_tw);
	printf("\t-S:\t\tUse SQPOLL (%d)\n", sqpoll);
	printf("\t-b:\t\tSend/receive buf size (%d)\n", buf_size);
	printf("\t-n:\t\tNumber of provided buffers (pow2) (%d)\n", nr_bufs);
	printf("\t-s:\t\tAct only as a sink (%d)\n", is_sink);
	printf("\t-f:\t\tUse only fixed files (%d)\n", fixed_files);
	printf("\t-B:\t\tUse bi-directional mode (%d)\n", bidi);
	printf("\t-H:\t\tHost to connect to (%s)\n", host);
	printf("\t-r:\t\tPort to receive on (%d)\n", receive_port);
	printf("\t-p:\t\tPort to connect to (%d)\n", send_port);
	printf("\t-6:\t\tUse IPv6 (%d)\n", ipv6);
	printf("\t-V:\t\tIncrease verbosity (%d)\n", verbose);
}

static void check_for_close(struct io_uring *ring)
{
	int i;

	for (i = 0; i < nr_conns; i++) {
		struct conn *c = &conns[i];

		if (c->flags & (CONN_F_DISCONNECTING | CONN_F_DISCONNECTED))
			continue;
		if (pending_shutdown(c)) {
			__close_conn(ring, c);
			c->flags |= CONN_F_DISCONNECTING;
		}
	}
}

int main(int argc, char *argv[])
{
	struct io_uring_sqe *sqe;
	struct io_uring ring;
	struct io_uring_params params;
	struct sigaction sa = { };
	int opt, ret, fd;

	while ((opt = getopt(argc, argv, "m:d:S:s:b:f:H:r:p:n:B:6Vh?")) != -1) {
		switch (opt) {
		case 'm':
			mshot = !!atoi(optarg);
			break;
		case 'S':
			sqpoll = !!atoi(optarg);
			break;
		case 'd':
			defer_tw = !!atoi(optarg);
			break;
		case 'b':
			buf_size = atoi(optarg);
			break;
		case 'n':
			nr_bufs = atoi(optarg);
			break;
		case 's':
			is_sink = !!atoi(optarg);
			break;
		case 'f':
			fixed_files = !!atoi(optarg);
			break;
		case 'H':
			host = strdup(optarg);
			break;
		case 'r':
			receive_port = atoi(optarg);
			break;
		case 'p':
			send_port = atoi(optarg);
			break;
		case 'B':
			bidi = !!atoi(optarg);
			break;
		case '6':
			ipv6 = true;
			break;
		case 'V':
			verbose++;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (bidi && is_sink) {
		fprintf(stderr, "Can't be both bidi proxy and sink\n");
		return 1;
	}

	br_mask = nr_bufs - 1;

	if (is_sink) {
		fd = setup_listening_socket(send_port);
		receive_port = -1;
	} else {
		fd = setup_listening_socket(receive_port);
	}

	if (fd == -1)
		return 1;

	atexit(show_stats);
	sa.sa_handler = sig_int;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGINT, &sa, NULL);

	/*
	 * By default, set us up with a big CQ ring. Not strictly needed
	 * here, but it's very important to never overflow the CQ ring.
	 * Events will not be dropped if this happens, but it does slow
	 * the application down in dealing with overflown events.
	 *
	 * Set SINGLE_ISSUER, which tells the kernel that only one thread
	 * is doing IO submissions. This enables certain optimizations in
	 * the kernel.
	 */
	memset(&params, 0, sizeof(params));
	params.flags |= IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_CLAMP;
	params.flags |= IORING_SETUP_CQSIZE;
	params.cq_entries = 131072;

	/*
	 * DEFER_TASKRUN decouples async event reaping and retrying from
	 * regular system calls. If this isn't set, then io_uring uses
	 * normal task_work for this. task_work is always being run on any
	 * exit to userspace. Real applications do more than just call IO
	 * related system calls, and hence we can be running this work way
	 * too often. Using DEFER_TASKRUN defers any task_work running to
	 * when the application enters the kernel anyway to wait on new
	 * events. It's generally the preferred and recommended way to setup
	 * a ring.
	 */
	if (defer_tw) {
		params.flags |= IORING_SETUP_DEFER_TASKRUN;
		sqpoll = 0;
	}

	/*
	 * SQPOLL offloads any request submission and retry operations to a
	 * dedicated thread. This enables an application to do IO without
	 * ever having to enter the kernel itself. The SQPOLL thread will
	 * stay busy as long as there's work to do, and go to sleep if
	 * sq_thread_idle msecs have passed. If it's running, submitting new
	 * IO just needs to make them visible to the SQPOLL thread, it needs
	 * not enter the kernel. For submission, the application will only
	 * enter the kernel if the SQPOLL has been idle long enough that it
	 * has gone to sleep.
	 *
	 * Waiting on events still need to enter the kernel, if none are
	 * available. The application may also use io_uring_peek_cqe() to
	 * check for new events without entering the kernel, as completions
	 * will be continually produced to the CQ ring by the SQPOLL thread
	 * as they occur.
	 */
	if (sqpoll) {
		params.flags |= IORING_SETUP_SQPOLL;
		params.sq_thread_idle = 1000;
		defer_tw = 0;
	}

	/*
	 * If neither DEFER_TASKRUN or SQPOLL is used, set COOP_TASKRUN. This
	 * avoids heavy signal based notifications, which can force an
	 * application to enter the kernel and process it as soon as they
	 * occur.
	 */
	if (!sqpoll && !defer_tw)
		params.flags |= IORING_SETUP_COOP_TASKRUN;

	/*
	 * The SQ ring size need not be larger than any batch of requests
	 * that need to be prepared before submit. Normally in a loop we'd
	 * only need a few, if any, particularly if multishot is used.
	 */
	ret = io_uring_queue_init_params(128, &ring, &params);
	if (ret) {
		fprintf(stderr, "%s\n", strerror(-ret));
		return 1;
	}

	if (fixed_files) {
		/*
		 * If fixed files are used, we need to allocate a fixed file
		 * table upfront where new direct descriptors can be managed.
		 */
		ret = io_uring_register_files_sparse(&ring, 4096);
		if (ret) {
			fprintf(stderr, "file register: %d\n", ret);
			return 1;
		}

		/*
		 * If fixed files are used, we also register the ring fd. See
		 * comment near io_uring_prep_socket_direct_alloc() further
		 * down. This avoids the fget/fput overhead associated with
		 * the io_uring_enter(2) system call itself, which is used to
		 * submit and wait on events.
		 */
		ret = io_uring_register_ring_fd(&ring);
		if (ret != 1) {
			fprintf(stderr, "ring register: %d\n", ret);
			return 1;
		}
	}

	printf("Backend: multishot=%d, sqpoll=%d, defer_tw=%d, fixed_files=%d "
		"is_sink=%d, buf_size=%d, nr_bufs=%d, host=%s, send_port=%d "
		"receive_port=%d\n",
			mshot, sqpoll, defer_tw, fixed_files, is_sink,
			buf_size, nr_bufs, host, send_port, receive_port);

	/*
	 * proxy provides a way to use either multishot receive or not, but
	 * for accept, we always use multishot. A multishot accept request
	 * needs only be armed once, and then it'll trigger a completion and
	 * post a CQE whenever a new connection is accepted. No need to do
	 * anything else, unless the multishot accept terminates. This happens
	 * if it encounters an error. Applications should check for
	 * IORING_CQE_F_MORE in cqe->flags - this tells you if more completions
	 * are expected from this request or not. Non-multishot never have
	 * this set, where multishot will always have this set unless an error
	 * occurs.
	 */
	sqe = get_sqe(&ring);
	if (fixed_files)
		io_uring_prep_multishot_accept_direct(sqe, fd, NULL, NULL, 0);
	else
		io_uring_prep_multishot_accept(sqe, fd, NULL, NULL, 0);
	__encode_userdata(sqe, 0, 0, 0, 0, fd);

	while (1) {
		struct __kernel_timespec ts = {
			.tv_sec = 0,
			.tv_nsec = 100000000ULL,
		};
		struct io_uring_cqe *cqe;
		unsigned int head;
		unsigned int i = 0;
		int to_wait;

		to_wait = 1;
		if (nr_conns)
			to_wait = nr_conns;

		io_uring_submit_and_wait_timeout(&ring, &cqe, to_wait, &ts, NULL);

		io_uring_for_each_cqe(&ring, head, cqe) {
			if (handle_cqe(&ring, cqe))
				return 1;
			++i;
		}

		/*
		 * Advance the CQ ring for seen events when we've processed
		 * all of them in this loop. This can also be done with
		 * io_uring_cqe_seen() in each handler above, which just marks
		 * that single CQE as seen. However, it's more efficient to
		 * mark a batch as seen when we're done with that batch.
		 */
		if (i)
			io_uring_cq_advance(&ring, i);
		else
			check_for_close(&ring);
	}

	return 0;
}
