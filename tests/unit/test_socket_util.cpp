#include <gtest/gtest.h>
#include "server/SocketUtil.h"

#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>

// forceSocketShutdown() must reliably unblock a thread parked in a
// synchronous, blocking read() on the socket — this is the core primitive
// the StreamingSession idle-watchdog and HandshakeWatchdog both depend on
// (jota-transcriber#68: SO_RCVTIMEO alone does not reliably interrupt a
// blocking read on a Boost.Asio-managed socket).
TEST(SocketUtil, ForceSocketShutdownUnblocksPendingRead) {
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));

    std::atomic<bool> read_returned{false};
    std::atomic<ssize_t> read_result{-2};

    std::thread reader([&]() {
        char buf[16];
        ssize_t n = read(fds[0], buf, sizeof(buf));
        read_result = n;
        read_returned = true;
    });

    // Give the reader thread time to actually block in read() before we shut it down.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_FALSE(read_returned.load()) << "reader returned before shutdown — test setup is wrong";

    forceSocketShutdown(fds[0]);

    reader.join();
    EXPECT_TRUE(read_returned.load());
    EXPECT_EQ(0, read_result.load()) << "expected EOF (0) from a shut-down read, not an error or data";

    close(fds[0]);
    close(fds[1]);
}
