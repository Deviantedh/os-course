#define _GNU_SOURCE
#include <errno.h>
#include <linux/sched.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

pid_t do_clone3_or_fork(void) {
  struct clone_args args;
  memset(&args, 0, sizeof(args));

  args.exit_signal = SIGCHLD;

  long ret = syscall(SYS_clone3, &args, sizeof(args));
  if (ret == -1) {
    if (errno == ENOSYS) {
      return fork();
    }
    return -1;
  }
  return (pid_t)ret;
}