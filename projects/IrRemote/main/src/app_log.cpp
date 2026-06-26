#include "app_log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace applog {
namespace {

std::mutex g_log_mutex;
std::string g_log_path;
bool g_initialized = false;

std::string trim_dirname(const std::string &path)
{
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
    {
        return ".";
    }
    if (slash == 0)
    {
        return "/";
    }
    return path.substr(0, slash);
}

void ensure_dir(const std::string &path)
{
    if (path.empty())
    {
        return;
    }

    std::string current;
    for (char ch : path)
    {
        current.push_back(ch);
        if (ch == '/' && current != "/")
        {
            mkdir(current.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

std::string resolve_log_path()
{
    const char *forced = std::getenv("IRREMOTE_LOG_PATH");
    if (forced != nullptr && forced[0] != '\0')
    {
        return forced;
    }

    const char *home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0')
    {
        return std::string(home) + "/.config/m5cardputerzero/irremote/runtime.log";
    }

    return "/tmp/irremote-runtime.log";
}

void init_once_locked()
{
    if (g_initialized)
    {
        return;
    }

    g_log_path = resolve_log_path();
    ensure_dir(trim_dirname(g_log_path));
    g_initialized = true;
}

std::string timestamp_now()
{
    std::time_t now = std::time(nullptr);
    std::tm local_tm {};
    localtime_r(&now, &local_tm);
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    return buffer;
}

void write_line_locked(const char *level, const std::string &message)
{
    init_once_locked();

    const std::string line = "[" + timestamp_now() + "] [" + level + "] " + message;
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);

    FILE *fp = std::fopen(g_log_path.c_str(), "a");
    if (fp == nullptr)
    {
        std::fprintf(stderr, "[app_log] failed to open %s: %s\n", g_log_path.c_str(), std::strerror(errno));
        return;
    }

    std::fprintf(fp, "%s\n", line.c_str());
    std::fclose(fp);
}

void write_line(const char *level, const std::string &message)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    write_line_locked(level, message);
}

}  // namespace

void begin_session(const std::string &title)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    init_once_locked();
    write_line_locked("INFO", "===== " + title + " =====");
    write_line_locked("INFO", "log_path=" + g_log_path);
}

void debug(const std::string &message)
{
    write_line("DEBUG", message);
}

void info(const std::string &message)
{
    write_line("INFO", message);
}

void warn(const std::string &message)
{
    write_line("WARN", message);
}

void error(const std::string &message)
{
    write_line("ERROR", message);
}

std::string log_path()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    init_once_locked();
    return g_log_path;
}

}  // namespace applog
