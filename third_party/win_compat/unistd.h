// M19: stub <unistd.h> for MSVC so the vendored UxPlay core's
//   #include <unistd.h>
// resolves. usleep/clock_gettime come from the pthread shim; the socket-close
// and misc bits the core needs are pulled from Winsock via compat.h.
#ifndef VKR_WIN_UNISTD_H
#define VKR_WIN_UNISTD_H
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include "pthread.h" // usleep/clock_gettime shims
#endif
#endif
