// pthreads on top of host threads. Each guest thread is a host thread with its
// own JIT. Locks keep a pointer to a host object inside the guest structure.
#include "libc.h"
#include "platform.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

namespace {
// bionic LP64 layouts: pthread_mutex_t is 40 bytes, pthread_cond_t 48, sem_t 16.
// The first 4 bytes hold bionic's own state (static initialisers set type bits
// there); we keep our host object pointer at offset 8.
struct HMutex { CRITICAL_SECTION cs; };
struct HCond { CONDITION_VARIABLE cv; };

template <class T>
T* host_object(void* guest, void (*init)(T*)) {
    void** slot = reinterpret_cast<void**>(static_cast<u8*>(guest) + 8);
    void* cur = InterlockedCompareExchangePointer(slot, nullptr, nullptr);
    if (cur) return static_cast<T*>(cur);
    T* fresh = new T;
    init(fresh);
    void* prev = InterlockedCompareExchangePointer(slot, fresh, nullptr);
    if (prev) { delete fresh; return static_cast<T*>(prev); }   // another thread won the race
    return fresh;
}
void init_mutex(HMutex* m) { InitializeCriticalSection(&m->cs); }
void init_cond(HCond* c) { InitializeConditionVariable(&c->cv); }
HMutex* mutex_of(void* g) { return host_object<HMutex>(g, init_mutex); }
HCond* cond_of(void* g) { return host_object<HCond>(g, init_cond); }
void clear_slot(void* guest) { memset(static_cast<u8*>(guest) + 8, 0, 8); }

std::mutex g_threads_mutex;
std::map<u64, Thread*> g_threads;
std::atomic<u32> g_next_key{1};
std::atomic<int> g_thread_count{0};

struct Timespec { i64 sec, nsec; };

int cond_wait(void* cond, void* mutex, const Timespec* abs) {
    DWORD ms = INFINITE;
    if (abs) {
        using namespace std::chrono;
        i64 now = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        i64 left = abs->sec * 1000000000 + abs->nsec - now;
        ms = left <= 0 ? 0 : static_cast<DWORD>(std::min<i64>((left + 999999) / 1000000, 0x7FFFFFFF));
    }
    if (SleepConditionVariableCS(&cond_of(cond)->cv, &mutex_of(mutex)->cs, ms)) return 0;
    return 110;   // ETIMEDOUT
}
}  // namespace

void register_pthread() {
    IMPORT("pthread_mutex_init", [](void* m, void*) -> int { clear_slot(m); return 0; });
    IMPORT("pthread_mutex_destroy", [](void* m) -> int { return 0; });
    IMPORT("pthread_mutex_lock", [](void* m) -> int { EnterCriticalSection(&mutex_of(m)->cs); return 0; });
    IMPORT("pthread_mutex_trylock", [](void* m) -> int { return TryEnterCriticalSection(&mutex_of(m)->cs) ? 0 : 16; });
    IMPORT("pthread_mutex_unlock", [](void* m) -> int { LeaveCriticalSection(&mutex_of(m)->cs); return 0; });
    IMPORT("pthread_cond_init", [](void* c, void*) -> int { clear_slot(c); return 0; });
    IMPORT("pthread_cond_destroy", [](void*) -> int { return 0; });
    IMPORT("pthread_cond_signal", [](void* c) -> int { WakeConditionVariable(&cond_of(c)->cv); return 0; });
    IMPORT("pthread_cond_broadcast", [](void* c) -> int { WakeAllConditionVariable(&cond_of(c)->cv); return 0; });
    IMPORT("pthread_cond_wait", [](void* c, void* m) -> int { return cond_wait(c, m, nullptr); });
    IMPORT("pthread_cond_timedwait", [](void* c, void* m, const Timespec* ts) -> int { return cond_wait(c, m, ts); });

    // sem_t: the count lives in the guest structure itself (first 4 bytes) and
    // waiters sleep on that address, so zero-initialised or copied semaphores work.
    IMPORT("sem_init", [](std::atomic<u32>* s, int, unsigned value) -> int { s->store(value); return 0; });
    IMPORT("sem_destroy", [](void*) -> int { return 0; });
    IMPORT("sem_post", [](std::atomic<u32>* s) -> int { s->fetch_add(1); WakeByAddressSingle(s); return 0; });
    IMPORT("sem_wait", [](std::atomic<u32>* s) -> int {
        for (;;) {
            u32 c = s->load();
            if (c > 0) { if (s->compare_exchange_weak(c, c - 1)) return 0; continue; }
            u32 zero = 0;
            WaitOnAddress(s, &zero, sizeof zero, INFINITE);
        }
    });
    IMPORT("sem_trywait", [](std::atomic<u32>* s) -> int {
        u32 c = s->load();
        while (c > 0) if (s->compare_exchange_weak(c, c - 1)) return 0;
        *guest_errno() = 11;
        return -1;
    });

    // attributes: pthread_attr_t { u32 flags; void* stack_base; size_t stack_size; ... } (56 bytes)
    IMPORT("pthread_attr_init", [](void* a) -> int { memset(a, 0, 56); return 0; });
    IMPORT("pthread_attr_setstacksize", [](void* a, size_t n) -> int { memcpy(static_cast<u8*>(a) + 16, &n, 8); return 0; });
    for (const char* n : {"pthread_attr_destroy", "pthread_attr_setdetachstate", "pthread_mutexattr_init",
                          "pthread_mutexattr_destroy", "pthread_mutexattr_settype", "pthread_detach",
                          "pthread_setschedparam", "pthread_key_delete", "sched_get_priority_min"})
        guest::reg(n, [](Thread& t) { t.setx(0, 0); });
    IMPORT("pthread_getschedparam", [](u64, int* policy, int* param) -> int { *policy = 0; *param = 0; return 0; });

    IMPORT("pthread_create", [](Thread& t, u64* out, void* attr, u64 start, u64 arg) -> int {
        size_t stack = 0;
        if (attr) memcpy(&stack, static_cast<u8*>(attr) + 16, 8);
        std::string name = guest::symbolize(start);
        name = name.substr(0, name.find('+'));
        // The first thread the game creates is its main loop; it runs most of the code.
        bool first = g_thread_count++ == 0;
        auto* nt = new Thread(name, stack, first ? 256 : 8);
        {
            std::lock_guard lk(g_threads_mutex);
            g_threads[nt->tid] = nt;
        }
        if (out) *out = nt->tid;
        logf("[thread] %d started: %s (process memory %.0f MB)", nt->tid, name.substr(0, 70).c_str(), guest::private_mb());
        nt->host_thread = new std::thread([nt, start, arg, first] {
            if (first) {
                HANDLE h;
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
                platform::set_game_thread_handle(h);
            }
            nt->make_current();
            nt->exit_value = nt->call(start, {arg});
            nt->exited = true;
        });
        return 0;
    });
    IMPORT("pthread_join", [](u64 tid, u64* ret) -> int {
        Thread* th;
        {
            std::lock_guard lk(g_threads_mutex);
            auto it = g_threads.find(tid);
            if (it == g_threads.end()) return 3;
            th = it->second;
        }
        auto* ht = static_cast<std::thread*>(th->host_thread);
        if (ht && ht->joinable()) ht->join();
        if (ret) *ret = th->exit_value;
        return 0;
    });
    IMPORT("pthread_exit", [](Thread& t, u64 value) { t.exit_value = value; t.exited = true; });
    IMPORT("pthread_self", [](Thread& t) -> u64 { return t.tid; });
    IMPORT("gettid", [](Thread& t) -> int { return t.tid; });
    IMPORT("pthread_equal", [](u64 a, u64 b) -> int { return a == b; });

    IMPORT("pthread_once", [](Thread& t, std::atomic<i32>* once, u64 fn) -> int {
        i32 expected = 0;
        if (once->compare_exchange_strong(expected, 1)) {
            t.call(fn);
            once->store(2);
        } else {
            while (once->load() != 2) std::this_thread::yield();
        }
        return 0;
    });
    IMPORT("pthread_key_create", [](u32* key, u64) -> int { *key = g_next_key++; return 0; });
    IMPORT("pthread_setspecific", [](Thread& t, u32 key, u64 value) -> int {
        if (t.tls.size() <= key) t.tls.resize(key + 16);
        t.tls[key] = value;
        return 0;
    });
    IMPORT("pthread_getspecific", [](Thread& t, u32 key) -> u64 { return key < t.tls.size() ? t.tls[key] : 0; });
}
