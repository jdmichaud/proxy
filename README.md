# proxy

https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c compiled using zig build system.

All credit to the author of `proxy.c` Jens Axboe.

# Build

```bash
zig build
```

To build a small static version:
```bash
zig build -Dtarget=x86_64-linux-musl -Doptimize=ReleaseSmall
```

# Usage

Redirect port 4022 to port 22 in Bidirectional mode.
```bash
proxy -H 127.0.0.1 -B1 -p22 -r4022
```
Then:
```
ssh localhost -p 4022
```

