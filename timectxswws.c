#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/futex.h>

static inline long long unsigned time_ns(struct timespec* const ts) {
  if (clock_gettime(CLOCK_REALTIME, ts)) {
    exit(1);
  }
  return ((long long unsigned) ts->tv_sec) * 1000000000LLU
    + (long long unsigned) ts->tv_nsec;
}

static inline int get_iterations(unsigned long long int ws_pages) {
  unsigned long long int iterations = 1000;
  while (iterations * ws_pages * 4096L < 4294967296L) {  // 4GB
    iterations += 1000;
  }
  return iterations;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <size of working set in 4K pages>\n", *argv);
    return 1;
  }
  const long long ws_pages = strtol(argv[1], NULL, 10);
  if (ws_pages < 0) {
    fprintf(stderr, "Invalid usage: working set size must be positive\n");
    return 1;
  }
  const long long int iterations = get_iterations(ws_pages);
  struct timespec ts;

  long long unsigned memset_time = 0;
  if (ws_pages) {
    void* buf = malloc(ws_pages * 4096);
    memset_time = time_ns(&ts);
    for (int i = 0; i < iterations; i++) {
      memset(buf, i, ws_pages * 4096);
    }
    memset_time = time_ns(&ts) - memset_time;
    printf("%lli memset on %4lliK in %10lluns (%.1fns/page)\n",
           iterations, ws_pages * 4, memset_time,
           (memset_time / ((float) ws_pages * iterations)));
    free(buf);
  }

  const int shm_id = shmget(IPC_PRIVATE, (ws_pages + 1) * 4096,
                            IPC_CREAT | 0666);
  const pid_t other = fork();
  int* futex = shmat(shm_id, NULL, 0);
  void* ws = ((char *) futex) + 4096;
  *futex = 0xA;
  if (other == 0) {
    for (int i = 0; i < iterations; i++) {
      sched_yield();
      while (syscall(SYS_futex, futex, FUTEX_WAIT, 0xA, NULL, NULL, 42)) {
        // retry
        sched_yield();
      }
      *futex = 0xB;
      if (ws_pages) {
        memset(ws, i, ws_pages * 4096);
      }
      while (!syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 42)) {
        // retry
        sched_yield();
      }
    }
    return 0;
  }

  const long long unsigned start_ns = time_ns(&ts);
  for (int i = 0; i < iterations; i++) {
    *futex = 0xA;
    if (ws_pages) {
      memset(ws, i, ws_pages * 4096);
    }
    while (!syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 42)) {
      // retry
      sched_yield();
    }
    sched_yield();
    while (syscall(SYS_futex, futex, FUTEX_WAIT, 0xB, NULL, NULL, 42)) {
      // retry
      sched_yield();
    }
  }
  const long long unsigned delta = time_ns(&ts) - start_ns - memset_time * 2;

  const int nswitches = iterations * 4;
  printf("%i process context switches (wss:%4lliK) in %12lluns (%.1fns/ctxsw)\n",
         nswitches, ws_pages * 4, delta, (delta / (float) nswitches));
  wait(futex);
  return 0;
}
