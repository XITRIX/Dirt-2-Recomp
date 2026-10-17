/* Table emitted by lifter_testgen and consumed by the differential runner. */
#ifndef LIFTER_CASE_TABLE_H
#define LIFTER_CASE_TABLE_H

#include "recomp/x86.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guest layout shared by the generator and the runner. */
#define LT_CODE_BASE 0x00401000u /* case i lives at LT_CODE_BASE + i * LT_CODE_STRIDE */
#define LT_CODE_STRIDE 0x100u
#define LT_DATA_BASE 0x00100000u /* scratch memory, compared byte for byte */
#define LT_DATA_SIZE 0x10000u

typedef struct LifterCase {
    const char* name;
    const char* asm_text;
    const char* options;  /* harness options from cases.txt */
    uint32_t addr;        /* guest address of the first byte */
    uint32_t len;         /* bytes before the appended ret */
    const uint8_t* code;  /* len bytes plus the ret */
    recomp_fn live;       /* lifted with every flag live at ret (spilled to eflags) */
    recomp_fn dead;       /* default lifting: flags dead at ret */
    uint32_t undef_flags; /* arithmetic flags the snippet leaves undefined (Zydis) */
} LifterCase;

extern const LifterCase lifter_cases[];
extern const uint32_t lifter_case_count;

#ifdef __cplusplus
}
#endif

#endif
