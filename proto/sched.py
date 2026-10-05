"""Cooperative-preemptive guest threads: each thread is a saved CPU context,
run in short time slices on the single Unicorn instance. Blocking pthread
calls park the thread until a Python predicate says it can continue."""
import struct
import time

from unicorn import UcError
from unicorn import arm64_const as A

from emu import imp
from libc import T0

SLICE = 0.02             # seconds a thread runs before the next import call preempts it
FALLBACK_US = 1_000_000  # hard stop for a thread that computes without calling any import
THREAD_STACK = 0x100000
EBUSY, ETIMEDOUT = 16, 110


class Thread:
    def __init__(self, tid, name):
        self.tid, self.name = tid, name
        self.ctx = None            # saved Unicorn context once it has run
        self.init = None           # (pc, sp, args) before first run
        self.pc = 0
        self.done = True
        self.result = 0
        self.blocked = None        # predicate; thread sleeps until it returns True
        self.tls = {}
        self.cond = None           # state of an in-progress pthread_cond_wait
        self.stack = 0


class Scheduler:
    def __init__(self, e):
        self.e = e
        e.sched = self
        self.threads = []
        self.cur = None
        self.fault = False
        self.loaded = None         # thread whose registers are currently in the CPU
        self.mutexes = {}          # addr -> [owner tid, depth]
        self.sems = {}
        self.signals = {}          # cond addr -> wake counter
        self.next_tid = 1
        e.stub_fns[(e.exit_addr - 0x10000000) // 4] = self._thread_exit

    def new_thread(self, name):
        t = Thread(self.next_tid, name)
        self.next_tid += 1
        t.stack = self.e.malloc(THREAD_STACK, 0x1000)
        self.threads.append(t)
        return t

    def start(self, t, pc, *args):
        if self.loaded is t:
            self.loaded = None
        t.ctx, t.done, t.blocked, t.cond = None, False, None, None
        t.init = (pc, t.stack + THREAD_STACK - 0x100, args)
        t.pc = pc

    def _thread_exit(self, e):
        self.cur.done = True
        self.cur.result = e.x(0)
        e.uc.emu_stop()

    def block(self, predicate, resume_at=None):
        """Park the current thread. It resumes at the stub (re-running the
        handler) unless resume_at is given."""
        t = self.cur
        t.blocked = predicate
        t.resume_pc = resume_at
        self.e.uc.emu_stop()

    def maybe_preempt(self):
        """Called after every import handler: end the slice at this safe point."""
        t = self.cur
        if t.blocked is None and not t.done and time.perf_counter() > self.slice_end:
            t.resume_pc = self.e.lr
            self.e.uc.emu_stop()

    def _run_slice(self, t):
        e, uc = self.e, self.e.uc
        self.cur = t
        t.resume_pc = None
        if self.loaded is not None and self.loaded is not t:
            self.loaded.ctx = uc.context_save()      # CPU still holds the previous thread
        if t.ctx is None:
            t.ctx = True
            pc, sp, args = t.init
            for i, a in enumerate(args):
                e.setx(i, a)
            e.setx(29, 0)
            e.setx(30, e.exit_addr)
            uc.reg_write(A.UC_ARM64_REG_SP, sp)
        elif self.loaded is not t:
            uc.context_restore(t.ctx)
        self.loaded = t
        e.stub_done_pc = None
        self.slice_end = time.perf_counter() + SLICE
        try:
            uc.emu_start(t.pc, 0, timeout=FALLBACK_US)
        except UcError as err:
            print(f"\n** CPU fault in thread {t.tid} ({t.name}): {err} at pc={e.sym(e.pc)}")
            e.backtrace()
            self.fault = True
            return
        if t.blocked is not None and t.resume_pc is None and not 0x10000000 <= e.pc < 0x10100000:
            print(f"  !! thread {t.tid} blocked but stopped outside a stub at {e.sym(e.pc)}")
        if t.resume_pc is not None:
            t.pc = t.resume_pc
        elif t.blocked is None and e.pc == e.stub_done_pc:
            t.pc = e.lr          # stopped after the handler ran but before the stub's `ret`
        else:
            t.pc = e.pc

    def run(self, until=None, wall=None, tick=None):
        """Run threads until `until` finishes, `wall` seconds pass, or a fault."""
        end = time.time() + wall if wall else None
        next_tick = time.time() + 10
        while not self.fault:
            if until is not None and until.done:
                return True
            now = time.time()
            if end and now > end:
                return False
            if tick and now > next_tick:
                tick()
                next_tick = now + 10
            ran = False
            for t in list(self.threads):
                if t.done or self.fault:
                    continue
                if t.blocked is not None:
                    if not t.blocked():
                        continue
                    t.blocked = None
                self._run_slice(t)
                ran = True
                if until is not None and until.done:
                    return True
            if not ran:
                if all(t.done for t in self.threads):
                    return False
                time.sleep(0.001)
        return False

    def call(self, t, pc, *args, wall=None):
        self.start(t, pc, *args)
        ok = self.run(until=t, wall=wall)
        return t.result if ok else None

    def status(self):
        for t in self.threads:
            state = "done" if t.done else "blocked" if t.blocked else "running"
            print(f"   thread {t.tid} {t.name:28} {state:8} pc={self.e.sym(t.pc)}")


# ---- pthread API ----------------------------------------------------------
def S(e):
    return e.sched


@imp("pthread_create")
def _create(e):
    s = S(e)
    t = s.new_thread(e.sym(e.x(2)).split("+")[0])
    s.start(t, e.x(2), e.x(3))
    print(f"  ** pthread_create -> thread {t.tid}: {t.name}")
    if e.x(0):
        e.w64(e.x(0), t.tid)
    return 0


@imp("pthread_self", "gettid")
def _self(e):
    return S(e).cur.tid


@imp("getpid")
def _getpid(e):
    return 4242


@imp("pthread_equal")
def _equal(e):
    return int(e.x(0) == e.x(1))


@imp("pthread_join")
def _join(e):
    s = S(e)
    target = next((t for t in s.threads if t.tid == e.x(0)), None)
    if target is None or target.done:
        if e.x(1) and target is not None:
            e.w64(e.x(1), target.result)
        return 0
    s.block(lambda: target.done)


@imp("pthread_exit")
def _pexit(e):
    S(e)._thread_exit(e)


@imp("pthread_once")
def _once(e):
    once, fn = e.x(0), e.x(1)
    if e.u32(once):
        return 0
    e.w32(once, 1)
    e.setx(16, fn)
    e.setx(17, e.lr)
    e.setx(30, e.thunk_call)     # the stub's `ret` lands in the thunk, which calls fn
    return None


NEXT_KEY = [1]


@imp("pthread_key_create")
def _key_create(e):
    e.w32(e.x(0), NEXT_KEY[0])
    NEXT_KEY[0] += 1
    return 0


@imp("pthread_key_delete")
def _key_delete(e):
    return 0


@imp("pthread_setspecific")
def _setspecific(e):
    S(e).cur.tls[e.x(0) & 0xFFFFFFFF] = e.x(1)
    return 0


@imp("pthread_getspecific")
def _getspecific(e):
    return S(e).cur.tls.get(e.x(0) & 0xFFFFFFFF, 0)


def try_lock(s, addr, tid):
    m = s.mutexes.get(addr)
    if m is None or m[1] == 0:
        s.mutexes[addr] = [tid, 1]
        return True
    if m[0] == tid:
        m[1] += 1
        return True
    return False


@imp("pthread_mutex_lock")
def _lock(e):
    s, addr = S(e), e.x(0)
    if try_lock(s, addr, s.cur.tid):
        return 0
    s.block(lambda: s.mutexes[addr][1] == 0)


@imp("pthread_mutex_trylock")
def _trylock(e):
    s = S(e)
    return 0 if try_lock(s, e.x(0), s.cur.tid) else EBUSY


@imp("pthread_mutex_unlock")
def _unlock(e):
    m = S(e).mutexes.get(e.x(0))
    if m and m[1] > 0:
        m[1] -= 1
    return 0


def deadline(e, ts_ptr):
    sec, nsec = struct.unpack("<qq", e.read(ts_ptr, 16))
    t = sec + nsec / 1e9
    # Absolute deadline: realtime if it looks like an epoch value, else our monotonic clock.
    return t if t > 1e9 else time.time() + (t - (time.perf_counter() - T0 + 1000.0))


def cond_wait(e, timed):
    s, t = S(e), S(e).cur
    cond, mutex = e.x(0), e.x(1)
    if t.cond is None:                       # first entry: release the mutex and wait
        m = s.mutexes.get(mutex)
        depth = m[1] if m and m[0] == t.tid else 0
        if depth:
            m[1] = 0
        t.cond = (s.signals.get(cond, 0), depth, deadline(e, e.x(2)) if timed else None)
    seen, depth, until = t.cond
    signalled = s.signals.get(cond, 0) != seen
    expired = until is not None and time.time() >= until
    if not (signalled or expired):
        s.block(lambda: s.signals.get(cond, 0) != seen or (until is not None and time.time() >= until))
        return None
    m = s.mutexes.get(mutex)
    if depth and m and m[1] > 0 and m[0] != t.tid:          # must re-acquire first
        s.block(lambda: s.mutexes[mutex][1] == 0)
        return None
    if depth:
        s.mutexes[mutex] = [t.tid, depth]
    t.cond = None
    return 0 if signalled else ETIMEDOUT


@imp("pthread_cond_wait")
def _cond_wait(e):
    return cond_wait(e, False)


@imp("pthread_cond_timedwait")
def _cond_timedwait(e):
    return cond_wait(e, True)


@imp("pthread_cond_signal", "pthread_cond_broadcast")
def _cond_signal(e):
    s = S(e)
    s.signals[e.x(0)] = s.signals.get(e.x(0), 0) + 1
    return 0


@imp("sem_init")
def _sem_init(e):
    S(e).sems[e.x(0)] = e.x(2) & 0xFFFFFFFF
    return 0


@imp("sem_post")
def _sem_post(e):
    s = S(e)
    s.sems[e.x(0)] = s.sems.get(e.x(0), 0) + 1
    return 0


@imp("sem_wait")
def _sem_wait(e):
    s, addr = S(e), e.x(0)
    if s.sems.get(addr, 0) > 0:
        s.sems[addr] -= 1
        return 0
    s.block(lambda: s.sems.get(addr, 0) > 0)


@imp("sem_trywait")
def _sem_trywait(e):
    s, addr = S(e), e.x(0)
    if s.sems.get(addr, 0) > 0:
        s.sems[addr] -= 1
        return 0
    return 0xFFFFFFFFFFFFFFFF


def sleep_for(e, seconds):
    wake = time.time() + max(seconds, 0)
    e.setx(0, 0)
    S(e).block(lambda: time.time() >= wake, resume_at=e.lr)


@imp("usleep")
def _usleep(e):
    sleep_for(e, (e.x(0) & 0xFFFFFFFF) / 1e6)


@imp("nanosleep")
def _nanosleep(e):
    sec, nsec = struct.unpack("<qq", e.read(e.x(0), 16))
    sleep_for(e, sec + nsec / 1e9)


@imp("sched_yield")
def _yield(e):
    sleep_for(e, 0)


@imp("pthread_mutex_init", "pthread_mutex_destroy")
def _mutex_reset(e):
    S(e).mutexes.pop(e.x(0), None)
    return 0


for _n in ("pthread_mutexattr_init", "pthread_mutexattr_destroy",
           "pthread_mutexattr_settype", "pthread_cond_init", "pthread_cond_destroy", "pthread_attr_init",
           "pthread_attr_destroy", "pthread_attr_setdetachstate", "pthread_attr_setstacksize", "pthread_detach",
           "pthread_setschedparam", "pthread_getschedparam", "sem_destroy", "sched_get_priority_min"):
    imp(_n)(lambda e: 0)
