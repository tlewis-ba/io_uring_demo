# io_uring_demo

A small demo of Linux's io_uring.

# Instructions

The provided Makefile should work on a normal Linux.

On ubuntu, you need to install the `liburing-dev` dpkg for the
library, header, and man pages.

Sample usage:

```bash
$ ./write_io_uring foo 1024 8192 1024 0
$ ./write_io_uring foo 1024 8192 1024 1
```

Run it with `strace` to see the number of syscalls!
