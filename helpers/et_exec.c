/* Evil Tuner XXL -- et_exec: starts one helper program for the plugin, tied to the plugin's life.
 *
 *   et_exec <program> [args...]
 *
 * Before it becomes <program> it asks the kernel to kill it if the plugin's source thread goes away (PR_SET_PDEATHSIG:
 * MPC quit, crashed or removed the plugin), and closes every file descriptor but stdin/stdout/stderr, so a helper never
 * keeps MPC's own files, sockets or devices open. Fully static (musl): no dependency on MPC OS's libraries. */
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct rlimit rl;
    long fd, top = 1024;
    if (argc < 2) return 2;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() == 1) _exit(1);   /* the plugin already went away */
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < 65536) top = (long)rl.rlim_cur;
    for (fd = 3; fd < top; fd++) close((int)fd);
    signal(SIGPIPE, SIG_DFL);
    execv(argv[1], argv + 1);
    _exit(127);
}
