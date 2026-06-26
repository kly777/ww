#pragma once

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#endif

// ---- 统一日志宏 + 初始化 ----

#ifdef RELEASE
#define LOG(fmt, ...) ((void)0)
static inline void LogInit() {}
static inline void LogCleanup() {}
#else

inline FILE* g_logFile = nullptr;

#define LOG(fmt, ...)                                                          \
    do {                                                                       \
        printf(fmt, ##__VA_ARGS__);                                            \
        if (g_logFile) {                                                       \
            fprintf(g_logFile, fmt, ##__VA_ARGS__);                            \
            fflush(g_logFile);                                                 \
        }                                                                      \
        fflush(stdout);                                                        \
    } while (0)

static inline void
LogInit()
{
    // 初始化文件日志：输出到 exe 同目录下的 ww.log
    wchar_t logPath[MAX_PATH];
    GetModuleFileNameW(NULL, logPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(logPath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(logPath, MAX_PATH, L"ww.log");
        HANDLE hFile = CreateFileW(logPath,
                                   FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL,
                                   OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            int fd = _open_osfhandle((intptr_t)hFile, 0);
            if (fd != -1)
                g_logFile = _fdopen(fd, "a");
        }
    }
}

static inline void
LogCleanup()
{
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

#endif // RELEASE
