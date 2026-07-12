#include "mc_log.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

#include <QtCore/QTextStream>

#ifdef _WIN32
#include <windows.h>
#endif

static McLogLevel g_level = MC_LOG_INFO;
static std::ofstream g_file;
static std::mutex g_log_mtx;
static int g_progress_active = 0;

static const char *level_names[] = { "DEBUG", "INFO", "WARN", "ERROR" };

void mc_log_set_level(McLogLevel level) { g_level = level; }

void mc_log_set_progress(int active) {
    std::lock_guard<std::mutex> lock(g_log_mtx);
    g_progress_active = active;
}

void mc_log_set_file(const char *path) {
    if (g_file.is_open()) g_file.close();
    if (path) g_file.open(path, std::ios::app);
}

static void write_console(const char *str) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, str, -1, nullptr, 0);
        if (wlen > 0) {
            wchar_t *wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
            if (wbuf) {
                MultiByteToWideChar(CP_UTF8, 0, str, -1, wbuf, wlen);
                WriteConsoleW(h, wbuf, wlen - 1, nullptr, nullptr);
                free(wbuf);
                fflush(stdout);
                return;
            }
        }
    }
    // Fallback for piped/redirected output
    fputs(str, stdout);
    fflush(stdout);
#else
    // On non-Windows, use Qt's QTextStream with UTF-8
    static QTextStream ts(stdout, QIODevice::WriteOnly);
    ts.setCodec("UTF-8");
    ts << QString::fromUtf8(str) << Qt::flush;
#endif
}

void mc_console_write(const char *str) {
    write_console(str);
}

void mc_console_init(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

void mc_console_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    write_console(buf);
}

void mc_log(McLogLevel level, const char *fmt, ...) {
    if (level < g_level) return;
    std::lock_guard<std::mutex> lock(g_log_mtx);
    if (g_progress_active)
        write_console("\r                                                                                \r");
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);
    const char *lvl = (level >= 0 && level <= 3) ? level_names[level] : "?";
    va_list ap;
    va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    char line[4160];
    snprintf(line, sizeof(line), "[%s] %s: %s\n", timebuf, lvl, buf);
    write_console(line);
    if (g_file.is_open()) {
        g_file << "[" << timebuf << "] " << lvl << ": " << buf << std::endl;
    }
}
