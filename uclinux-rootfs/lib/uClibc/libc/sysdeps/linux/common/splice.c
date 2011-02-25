/* vi: set sw=4 ts=4: */
/*
 * splice() for uClibc
 *
 * Copyright (C) 2010 Broadcom Corp.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 */

#include "syscalls.h"
#include <sys/time.h>

libc_hidden_proto(splice)
_syscall6(int, splice,  int, fd_in, off_t *, off_in, int,  fd_out,  off_t *, off_out, size_t, len, int, flags);
libc_hidden_def(splice)
