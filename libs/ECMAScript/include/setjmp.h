#pragma once
typedef long jmp_buf[8];
#define setjmp(b) __builtin_setjmp(b)
#define longjmp(b, v) __builtin_longjmp(b, 1)
