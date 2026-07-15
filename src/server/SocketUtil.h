#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

// Forcibly interrupts a thread blocked in a synchronous read/write on this
// socket, from a *different* thread, by shutting it down at the protocol
// level in both directions. A pending blocking read returns 0 (EOF); a
// pending/future write fails with EPIPE/ECONNRESET.
//
// Deliberately does NOT close() the descriptor: close() from a thread that
// doesn't own the socket object races with fd reuse (the number can be
// handed to a brand-new, unrelated connection before the blocked thread
// notices). shutdown() carries no such risk — the fd stays valid until the
// owning thread's normal cleanup path closes it after observing the error.
//
// This is the fix for jota-transcriber#68: SO_RCVTIMEO set on the native
// handle does not reliably interrupt a synchronous Boost.Asio/Beast read
// (Asio treats the resulting EWOULDBLOCK as a spurious wakeup and retries
// instead of propagating a timeout).
#if defined(_WIN32)
inline void forceSocketShutdown(SOCKET fd) {
    ::shutdown(fd, SD_BOTH);
}
#else
inline void forceSocketShutdown(int fd) {
    ::shutdown(fd, SHUT_RDWR);
}
#endif
