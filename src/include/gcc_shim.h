// gcc_shim.h —— 仅给第三方库 mhook.cpp 用的兼容垫片（通过 -include 强制引入）
#pragma once
#include <stddef.h>

// MinGW.org 的 windef.h 不定义 max/min
#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif
