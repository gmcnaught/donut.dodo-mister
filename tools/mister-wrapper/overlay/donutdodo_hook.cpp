/* Donut Dodo `main=` hook (after maldita.castilla-mister's maldita_hook.cpp).
 *
 * MiSTer.ini `[DonutDodo] main=/media/fat/games/DonutDodo/MiSTer_DonutDodo`
 * makes stock Main_MiSTer exec this build when the DonutDodo core loads. This
 * build is upstream Main_MiSTer unchanged plus one call, inserted after the
 * scheduler's FPGA-readiness wait, that starts launch.sh. Loading any other core
 * re-execs stock MiSTer (its ini section has the default main=MiSTer).
 *
 * The call sits after scheduler_wait_fpga_ready() because Maldita measured a
 * wrapper that spawned the engine before that wait wedging the fabric on frame 1
 * in 3 of 5 launches (0 of 5 once moved here).
 */

#include "donutdodo_hook.h"
#include "donutdodo_child.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "user_io.h"

namespace {

/* The core's CONF_STR name, as written to /tmp/CORENAME. */
constexpr const char *kCoreName = "DonutDodo";
/* NOT _handler.sh: that name is MiSTer Frontier Master_Daemon's discovery
 * predicate, and the daemon would start a second engine on the same core load. */
constexpr const char *kHandler  = "/media/fat/games/DonutDodo/launch.sh";
constexpr const char *kLogDir   = "/media/fat/logs/DonutDodo";
constexpr const char *kLogPath  = "/media/fat/logs/DonutDodo/launch.log";
/* Touch to load the core with no engine (bisecting), no rebuild needed. */
constexpr const char *kNoEngineFlag = "/media/fat/games/DonutDodo/NOENGINE";

pid_t g_child = -1;
bool  g_decided = false;

/* write(2) to stderr and the launch log: MiSTer's stdio goes to /dev/console. */
void hlog(const char *msg)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "donutdodo_hook: %s\n", msg);
    if (n <= 0) return;
    size_t len = (size_t)(n >= (int)sizeof(buf) ? (int)sizeof(buf) - 1 : n);
    (void)!write(STDERR_FILENO, buf, len);
    int fd = open(kLogPath, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    (void)!write(fd, buf, len);
    close(fd);
}

void decide_and_spawn()
{
    mkdir("/media/fat/logs", 0755);
    mkdir(kLogDir, 0755);

    /* main= is per-core, so this only runs for our core unless a main= line was
     * put in a global section; then every core would start the engine. */
    const char *core = user_io_get_core_name();
    if (!core || strcmp(core, kCoreName) != 0)
    {
        hlog("not the DonutDodo core - running as stock MiSTer");
        return;
    }

    struct stat st;
    if (stat(kNoEngineFlag, &st) == 0)
    {
        hlog("NOENGINE flag present - not starting launch.sh");
        return;
    }

    char *const argv[] = { (char *)kHandler, NULL };
    g_child = donutdodo_child_spawn(argv, kLogPath);

    char buf[96];
    if (g_child < 0) snprintf(buf, sizeof(buf), "FAILED to spawn %s", kHandler);
    else snprintf(buf, sizeof(buf), "launch.sh spawned pid=%d", (int)g_child);
    hlog(buf);
}

} // namespace

void donutdodo_hook_poll(void)
{
    if (!g_decided)
    {
        /* One decision per exec, whatever it is: re-deciding could start a
         * second engine on the same fabric control block. */
        g_decided = true;
        decide_and_spawn();
        return;
    }

    if (g_child > 0)
    {
        int code = 0;
        if (donutdodo_child_reap(g_child, &code))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "launch.sh exited rc=%d", code);
            hlog(buf);
            g_child = -1;
        }
    }
}
