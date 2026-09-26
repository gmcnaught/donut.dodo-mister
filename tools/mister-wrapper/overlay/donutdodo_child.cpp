#include "donutdodo_child.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sched.h>

extern char **environ;

pid_t donutdodo_child_spawn(char *const argv[], const char *log_path)
{
    pid_t pid = fork();
    if (pid != 0) return pid;   /* parent, or -1 on failure */

    /* Own session: out of MiSTer's process group and controlling terminal. */
    setsid();

    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
    }
    if (log_path && *log_path) {
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            if (logfd > STDERR_FILENO) close(logfd);
        }
    }

    /* Upstream main() pins MiSTer to CPU1 and a fork inherits the mask;
     * launch.sh does its own CPU placement from a clean start. */
    cpu_set_t all_cpus;
    CPU_ZERO(&all_cpus);
    CPU_SET(0, &all_cpus);
    CPU_SET(1, &all_cpus);
    sched_setaffinity(0, sizeof(all_cpus), &all_cpus);

    execve(argv[0], argv, environ);
    _exit(127);
}

bool donutdodo_child_reap(pid_t pid, int *exit_code_out)
{
    int status;
    if (waitpid(pid, &status, WNOHANG) <= 0) return false;
    if (exit_code_out) {
        if (WIFEXITED(status)) *exit_code_out = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) *exit_code_out = 128 + WTERMSIG(status);
        else *exit_code_out = -1;
    }
    return true;
}
