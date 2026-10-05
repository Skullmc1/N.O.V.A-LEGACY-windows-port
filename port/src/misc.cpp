// Networking: reported as unavailable, so the game runs in its offline mode.
#include <atomic>
#include <chrono>
#include <thread>

#include "libc.h"

namespace {
int fake_fd() { static std::atomic<int> next{1000}; return next++; }
void nap(int timeout_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms < 0 || timeout_ms > 50 ? 50 : timeout_ms));
}
}  // namespace

void register_net() {
    for (const char* n : {"eventfd", "epoll_create", "epoll_create1"})
        guest::reg(n, [](Thread& t) { t.setx(0, fake_fd()); });
    IMPORT("pipe", [](int* fds) -> int { fds[0] = fake_fd(); fds[1] = fake_fd(); return 0; });
    for (const char* n : {"epoll_ctl", "fcntl", "ioctl", "setsockopt", "shutdown"})
        guest::reg(n, [](Thread& t) { t.setx(0, 0); });
    IMPORT("epoll_wait", [](int, void*, int, int timeout) -> int { nap(timeout); return 0; });
    IMPORT("poll", [](void*, u64, int timeout) -> int { nap(timeout); return 0; });
    IMPORT("select", [](int, void*, void*, void*, void*) -> int { nap(20); return 0; });
    IMPORT("getaddrinfo", [](const char* host, const char*, void*, void*) -> int {
        log_once(std::string("dns:") + (host ? host : ""), "[net] lookup %s -> no network", host);
        return 8;   // EAI_NONAME
    });
    IMPORT("gethostbyname", [](const char* host) -> void* {
        log_once(std::string("dns:") + (host ? host : ""), "[net] lookup %s -> no network", host);
        return nullptr;
    });
    IMPORT("freeaddrinfo", [](void*) {});
    IMPORT("gethostname", [](char* buf, size_t n) -> int { strncpy(buf, "localhost", n); return 0; });
    for (const char* n : {"socket", "connect", "bind", "listen", "accept", "recv", "recvfrom", "recvmsg", "send",
                          "sendto", "sendmsg", "sendfile", "getsockname", "getpeername", "getsockopt"})
        guest::reg(n, [](Thread& t) { *guest_errno() = 101; t.setx(0, ~0ull); });   // ENETUNREACH
}
