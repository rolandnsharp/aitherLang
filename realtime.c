/* realtime: put the calling thread on the audio core, at real-time
   priority, with memory locked. The engine's mix thread, tools/play and
   tools/card call aither_realtime once, at the start.

   The audio core is the highest-numbered core online (the uConsole's
   last A72 on mains, its last A53 on battery, when the A72s are off), or
   AITHER_RT_CPU. Priority is SCHED_FIFO AITHER_RT_PRIO (70); the kernel
   still keeps 5% of every second for everything else, so a runaway patch
   can't freeze the machine. Memory is locked only when RLIMIT_MEMLOCK is
   unlimited: with a small limit, locking future allocations would make
   later ones fail, which is worse than paging. AITHER_RT=0 does none of
   it. Without permission it says so once and carries on as before;
   notes/realtime-2026-10-19.md has the limits file that grants it.

   While it runs, the process also asks the kernel to keep every core out
   of idle states slower than AITHER_IDLE_US (100) microseconds to wake,
   by holding /dev/cpu_dma_latency open (the PM QoS request JACK and
   PipeWire make). On the uConsole's RK3399, a period interrupt that lands
   while the cluster sleeps waits up to 0.9 ms, and the es8388 clicks:
   speaker-test with 512-frame periods clicked every few seconds, and was
   clean holding this (2026-10-29). The request ends with the process.
   Opening the device needs the audio group's rule
   (~/uconsole/install/aither-realtime.sh); without it, it says so once.
   AITHER_IDLE_US=0 doesn't ask.

   A pinned thread whose core goes offline is moved off it by the kernel,
   so the engine calls aither_repin about once a second: it pins the
   thread again to whichever core is now the audio core. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <stdint.h>

static pid_t rt_tid;        /* the thread aither_realtime set up, 0 if none */
static int rt_cpu = -1;     /* the core it was last pinned to */
static int idle_fd = -1;    /* /dev/cpu_dma_latency, held open while the process runs */

/* Keep the cores out of idle states slower than AITHER_IDLE_US to wake.
   The request lasts as long as the file stays open: until the process ends. */
static const char *hold_idle(void) {
    const char *env = getenv("AITHER_IDLE_US");
    int32_t us = env && *env ? atoi(env) : 100;
    if (us <= 0) return "";
    if (idle_fd >= 0) return ", deep idle held off";
    int fd = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return "; deep idle not held off (no access to /dev/cpu_dma_latency: ~/uconsole/install/aither-realtime.sh)";
    if (write(fd, &us, sizeof us) != (ssize_t)sizeof us) { close(fd); return "; deep idle not held off"; }
    idle_fd = fd;
    return ", deep idle held off";
}

/* The highest-numbered core online, from "0-5" or "0-3,5". */
static int last_online(void) {
    FILE *f = fopen("/sys/devices/system/cpu/online", "r");
    if (!f) return -1;
    char buf[128] = {0};
    if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
    fclose(f);
    int best = -1, n = -1;
    for (char *p = buf;; p++) {
        if (*p >= '0' && *p <= '9') n = (n < 0 ? 0 : n * 10) + (*p - '0');
        else { if (n > best) best = n; n = -1; if (!*p) break; }
    }
    return best;
}

static int audio_cpu(void) {
    const char *env = getenv("AITHER_RT_CPU");
    return env && *env ? atoi(env) : last_online();
}

static int pin(pid_t tid, int cpu) {
    if (cpu < 0) return -1;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(tid, sizeof set, &set);
}

int aither_realtime(const char *who) {
    const char *off = getenv("AITHER_RT");
    if (off && strcmp(off, "0") == 0) return -1;
    rt_tid = (pid_t)syscall(SYS_gettid);
    rt_cpu = audio_cpu();
    if (pin(rt_tid, rt_cpu) != 0) rt_cpu = -1;
    const char *pe = getenv("AITHER_RT_PRIO");
    struct sched_param sp = { .sched_priority = pe && *pe ? atoi(pe) : 70 };
    /* RESET_ON_FORK: a child (card's command) starts at normal priority. */
    int fifo = sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &sp) == 0;
    struct rlimit ml;
    int locked = getrlimit(RLIMIT_MEMLOCK, &ml) == 0 && ml.rlim_cur == RLIM_INFINITY &&
                 mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
    const char *idle = hold_idle();
    if (!fifo)
        fprintf(stderr, "[%s] no real-time priority (rtprio limit): audio runs as before, "
                        "see notes/realtime-2026-10-19.md%s\n", who, idle);
    else
        fprintf(stderr, "[%s] real-time: SCHED_FIFO %d on core %d%s%s\n", who,
                sp.sched_priority, rt_cpu, locked ? ", memory locked" : "", idle);
    return rt_cpu;
}

void aither_repin(void) {
    if (rt_tid == 0 || getenv("AITHER_RT_CPU")) return;
    int cpu = last_online();
    if (cpu >= 0 && cpu != rt_cpu && pin(rt_tid, cpu) == 0) rt_cpu = cpu;
}
