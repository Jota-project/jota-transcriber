#include <gtest/gtest.h>
#include "server/HandshakeWatchdog.h"

#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <chrono>

// A raw TCP connection that completes the 3-way handshake but then sends
// nothing (or trickles data arbitrarily slowly) currently has zero timeout
// protection in handleSession(): the ConnectionGuard slot is acquired before
// any timeout exists, and the SSL handshake + initial HTTP read run fully
// unprotected. HandshakeWatchdog closes that gap: armed with a native socket
// handle and a deadline, it force-shuts the socket down if disarm() isn't
// called first.
TEST(HandshakeWatchdog, ForceClosesSocketIfNotDisarmedBeforeTimeout) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    HandshakeWatchdog watchdog(fds[0], /*timeout_sec=*/1);

    // Block on the peer end waiting for the watched fd to do *anything*.
    // Nobody ever writes — the only way this unblocks is the watchdog
    // force-shutting fds[0] down once its 1s deadline elapses.
    char buf[16];
    auto start = std::chrono::steady_clock::now();
    ssize_t n = read(fds[1], buf, sizeof(buf));
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(0, n) << "expected EOF from the watchdog forcing shutdown, got errno=" << errno;
    EXPECT_LT(elapsed, std::chrono::seconds(3))
        << "watchdog fired far later than its configured 1s deadline";

    close(fds[0]);
    close(fds[1]);
}

TEST(HandshakeWatchdog, DisarmPreventsForceClose) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    {
        HandshakeWatchdog watchdog(fds[0], /*timeout_sec=*/1);
        watchdog.disarm();
        // destructor runs here — must be a harmless no-op on an
        // already-disarmed watchdog.
    }

    // Wait past the original deadline to prove nothing fires late.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // The socket must still be fully alive: write on the watched fd, read
    // it back on the peer.
    const char msg = 'x';
    ASSERT_EQ(1, write(fds[0], &msg, 1));
    char buf[1] = {0};
    ASSERT_EQ(1, read(fds[1], buf, 1));
    EXPECT_EQ('x', buf[0]);

    close(fds[0]);
    close(fds[1]);
}
