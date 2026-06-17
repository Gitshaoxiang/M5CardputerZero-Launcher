#include "hal_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <pty.h>

struct hal_pty {
    int   master_fd;
    pid_t child_pid;
};

static const char *select_run_user()
{
    const char *user = getenv("SSHCLIENT_RUN_AS_USER");
    if (user && user[0] != '\0')
    {
        return user;
    }

    struct passwd *p = nullptr;
    setpwent();
    while ((p = getpwent()) != nullptr)
    {
        if (p->pw_uid >= 1000 && p->pw_uid < 65534 &&
            p->pw_shell && p->pw_shell[0] &&
            strstr(p->pw_shell, "nologin") == nullptr &&
            strstr(p->pw_shell, "/bin/false") == nullptr)
        {
            const char *result = strdup(p->pw_name);
            endpwent();
            return result;
        }
    }
    endpwent();
    return "pi";
}

hal_pty_t hal_pty_open(const char *cmd, const char *const *args,
                       int cols, int rows)
{
    int master_fd = -1;
    struct winsize ws = {};
    ws.ws_col = cols;
    ws.ws_row = rows;

    pid_t pid = forkpty(&master_fd, nullptr, nullptr, &ws);
    if (pid < 0)
    {
        return nullptr;
    }

    if (pid == 0)
    {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        if (getuid() == 0)
        {
            const char *username = select_run_user();
            struct passwd *pw = getpwnam(username);
            if (pw && strcmp(username, "root") != 0)
            {
                initgroups(pw->pw_name, pw->pw_gid);
                setgid(pw->pw_gid);
                setuid(pw->pw_uid);
                setenv("HOME", pw->pw_dir, 1);
                setenv("USER", pw->pw_name, 1);
                setenv("LOGNAME", pw->pw_name, 1);
                setenv("SHELL", pw->pw_shell[0] ? pw->pw_shell : "/bin/bash", 1);
                chdir(pw->pw_dir);
            }
        }

        if (args)
        {
            execvp(cmd, (char *const *)args);
        }
        else
        {
            execlp(cmd, cmd, (char *)nullptr);
        }
        _exit(127);
    }

    int flags = fcntl(master_fd, F_GETFL);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    struct hal_pty *pty = (struct hal_pty *)malloc(sizeof(struct hal_pty));
    if (pty == nullptr)
    {
        close(master_fd);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return nullptr;
    }
    pty->master_fd = master_fd;
    pty->child_pid = pid;
    return pty;
}

int hal_pty_read(hal_pty_t pty, char *buf, size_t buf_size)
{
    if (!pty) return -1;
    ssize_t n = read(pty->master_fd, buf, buf_size);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return (int)n;
}

int hal_pty_write(hal_pty_t pty, const char *buf, size_t len)
{
    if (!pty) return -1;
    return (int)write(pty->master_fd, buf, len);
}

int hal_pty_check_child(hal_pty_t pty, int *exit_status)
{
    if (!pty) return -1;
    int status = 0;
    pid_t r = waitpid(pty->child_pid, &status, WNOHANG);
    if (r == 0) return 0;
    if (r > 0)
    {
        if (exit_status) *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return 1;
    }
    return -1;
}

void hal_pty_close(hal_pty_t pty)
{
    if (!pty) return;
    kill(pty->child_pid, SIGKILL);
    waitpid(pty->child_pid, nullptr, 0);
    close(pty->master_fd);
    free(pty);
}
