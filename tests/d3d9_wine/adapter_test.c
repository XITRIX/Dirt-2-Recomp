/* CPU-only negative fixtures for reporting and fail-fast control flow. */
#include "wine/test.h"
#include "runner.h"
static const char* mode;
static void fixture(void) {
    ok(1,"initial assertion\n");
    if(!strcmp(mode,"fail"))ok(0,"quoted \"failure\"\n");
    if(!strcmp(mode,"todo")) { todo_wine ok(0,"upstream TODO\n"); }
    if(!strcmp(mode,"skip"))skip("capability unavailable\n");
    ok(1,"last assertion\n");
}
void wine_test_visual(void) { wine_run_case("visual.test_sanity",fixture); }
void wine_test_device(void) {}
void wine_test_stateblock(void) {}
void wine_test_d3d9ex(void) {}
int main(int argc,char**argv) {
    if(argc!=3)return 2;mode=argv[1];FILE*f=fopen(argv[2],"w");if(!f)return 2;
    wine_execute(0,f);fclose(f);return 0;
}
