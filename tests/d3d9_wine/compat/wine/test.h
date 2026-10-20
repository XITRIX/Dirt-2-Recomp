/* Native adapter for Wine tests. Upstream assertions remain unchanged. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stddef.h>
#include "platform_compat.h"
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define START_TEST(name) void wine_test_##name(void)
void wine_location(const char*,unsigned);
void wine_ok(int,const char*,...);
void wine_skip(const char*,...);
void wine_trace(const char*,...);
void wine_todo_begin(int);
int wine_todo_next(void);
void wine_run_case(const char*,void(*)(void));
const char* wine_dbgstr_rect(const RECT*);
#define ok(...) (wine_location(__FILE__,__LINE__),wine_ok(__VA_ARGS__))
#define ok_(f,l) (wine_location(f,l),wine_ok)
#define skip(...) (wine_location(__FILE__,__LINE__),wine_skip(__VA_ARGS__))
#define win_skip(...) skip(__VA_ARGS__)
#define trace(...) wine_trace(__VA_ARGS__)
#define broken(x) 0
#define todo_wine for(wine_todo_begin(1);wine_todo_next();)
#define todo_wine_if(x) for(wine_todo_begin(!!(x));wine_todo_next();)
#define flaky for(wine_todo_begin(0);wine_todo_next();)
#define flaky_wine flaky
void winetest_push_context(const char*,...);
void winetest_pop_context(void);
void wine_platform_skip(const char*);
