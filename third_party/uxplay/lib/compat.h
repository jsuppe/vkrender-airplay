/**
 *  Copyright (C) 2011-2012  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#ifndef COMPAT_H
#define COMPAT_H

#if defined(WIN32)
#include <ws2tcpip.h>
#include <windows.h>
/* MSVC's UCRT (>=1900) provides a real snprintf and errors if it is macro'd;
 * only the legacy MinGW/old-MSVC path needs the _snprintf alias. */
#if !defined(snprintf) && (!defined(_MSC_VER) || _MSC_VER < 1900)
#define snprintf _snprintf
#endif
/* Winsock has no SOL_TCP; IPPROTO_TCP is the setsockopt level for TCP_NODELAY. */
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#endif

#include "sockets.h"
#include "threads.h"

#endif
