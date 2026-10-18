// ============================================================================
// sr_compat.h —— MSVC / MinGW(GCC) 兼容层
// 只放"两种编译器写法不同"的东西，避免在业务代码里到处 #ifdef
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

// 线程局部存储：MSVC 用 __declspec(thread)，GCC 用 __thread
#if defined(_MSC_VER)
#  define SR_TLS __declspec(thread)
#else
#  define SR_TLS __thread
#endif

// MSVC 的"安全 CRT"函数在 MinGW.org 上不存在，统一走这两个包装
static inline void SRFormatV(wchar_t* buf, size_t cch, const wchar_t* fmt, va_list ap)
{
    if (!buf || cch == 0) return;
#if defined(_MSC_VER)
    _vsnwprintf_s(buf, cch, _TRUNCATE, fmt, ap);
#else
    _vsnwprintf(buf, cch, fmt, ap);
#endif
    buf[cch - 1] = L'\0';
}

static inline void SRFormat(wchar_t* buf, size_t cch, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SRFormatV(buf, cch, fmt, ap);
    va_end(ap);
}

// wcscat_s 的等价物（MinGW.org 没有 wcscat_s）
static inline void SRAppend(wchar_t* dst, size_t cch, const wchar_t* src)
{
    if (!dst || cch == 0 || !src) return;
    size_t n = wcslen(dst);
    if (n >= cch - 1) return;
    wcsncat(dst, src, cch - 1 - n);
}
