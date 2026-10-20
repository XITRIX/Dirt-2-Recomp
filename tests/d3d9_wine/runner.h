#pragma once
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
struct WineCase { const char *name,*module,*platform_block; };
unsigned wine_case_count(void);
const struct WineCase* wine_case_at(unsigned index);
int wine_execute(unsigned index,FILE* report);
#ifdef __cplusplus
}
#endif
