# proxy

Based on https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c

# Build

```bash
zig build
```

To build a small static version:
```bash
zig build -Dtarget=x86_64-linux-musl -Doptimize=ReleaseSmall
```

