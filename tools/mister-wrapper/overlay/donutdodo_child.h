#ifndef DONUTDODO_CHILD_H
#define DONUTDODO_CHILD_H

#include <sys/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the launch handler as a detached child (from maldita.castilla-mister's
 * maldita_child.cpp).
 *
 * argv[0] is the absolute path to exec; the child inherits environ. log_path
 * receives the child's stdout+stderr (O_APPEND); an unopenable path leaves the
 * parent's stdio, which under MiSTer is /dev/console.
 *
 * Detached on purpose: own session, no PR_SET_PDEATHSIG. On a core change
 * MiSTer execs stock Main_MiSTer in place of this process, and launch.sh's
 * watchdog (not this process) stops the engine; the fabric-gate core reload
 * also relies on launch.sh outliving the wrapper.
 *
 * Returns the child pid, or -1 if fork() failed. */
pid_t donutdodo_child_spawn(char *const argv[], const char *log_path);

/* WNOHANG reap. True when the child changed state; exit_code_out (if given) is
 * its exit status, or 128+signal if it was killed. */
bool  donutdodo_child_reap(pid_t pid, int *exit_code_out);

#ifdef __cplusplus
}
#endif

#endif /* DONUTDODO_CHILD_H */
