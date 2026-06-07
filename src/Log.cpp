#include "Log.h"

#include <BellLogger.h>

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <string>

static FILE *logger_fp = nullptr;
static std::mutex log_mutex;

class VitaFileLogger : public bell::AbstractLogger {
 public:
    void debug(std::string filename, int line, std::string submodule,
               const char *format, ...) override {
        va_list args;
        va_start(args, format);
        logLine("D", filename, line, submodule, format, args);
        va_end(args);
    }

    void error(std::string filename, int line, std::string submodule,
               const char *format, ...) override {
        va_list args;
        va_start(args, format);
        logLine("E", filename, line, submodule, format, args);
        va_end(args);
    }

    void info(std::string filename, int line, std::string submodule,
              const char *format, ...) override {
        va_list args;
        va_start(args, format);
        logLine("I", filename, line, submodule, format, args);
        va_end(args);
    }

 private:
    void logLine(const char *level, const std::string &filename, int line,
                 const std::string &submodule, const char *format, va_list args) {
        std::lock_guard<std::mutex> guard(log_mutex);
        if (logger_fp == nullptr) {
            return;
        }
        const auto slash = filename.find_last_of('/');
        const auto backslash = filename.find_last_of('\\');
        const auto pos = (slash == std::string::npos)
                             ? backslash
                             : (backslash == std::string::npos
                                    ? slash
                                    : std::max(slash, backslash));
        const char *base =
            pos == std::string::npos ? filename.c_str() : filename.c_str() + pos + 1;
        fprintf(logger_fp, "%s [%s] %s:%d: ", level, submodule.c_str(), base, line);
        vfprintf(logger_fp, format, args);
        fputc('\n', logger_fp);
        fflush(logger_fp);
    }
};

void init_logger() {
    logger_fp = fopen(LOG_FILE_NAME, "w");
    if (logger_fp != nullptr) {
        fprintf(logger_fp, "VitaSpotify log started\n");
        fflush(logger_fp);
    }
    bell::bellGlobalLogger = new VitaFileLogger();
}

void flush_logger() {
    std::lock_guard<std::mutex> guard(log_mutex);
    if (logger_fp != nullptr) {
        fflush(logger_fp);
    }
}

int print_to_menu(const char *fmt, ...) {
    std::lock_guard<std::mutex> guard(log_mutex);
    va_list args;
    va_start(args, fmt);
    if (logger_fp != nullptr) {
        vfprintf(logger_fp, fmt, args);
        fflush(logger_fp);
    }
    va_end(args);
    return 0;
}

int vprint_to_menu(const char *fmt, va_list args) {
    std::lock_guard<std::mutex> guard(log_mutex);
    if (logger_fp != nullptr) {
        vfprintf(logger_fp, fmt, args);
        fflush(logger_fp);
    }
    return 0;
}
