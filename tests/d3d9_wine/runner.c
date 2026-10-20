/* Original native reporting adapter; upstream tests retain their LGPL notices. */
#include "wine/test.h"
#include "runner.h"
#include <setjmp.h>
#include "inventory.inc"
static jmp_buf escape_case;
static const char *selected,*source;
static unsigned line,passed,failed,skipped,todo_failed;
static int todo, todo_pending,entered,platform_skip;
static char message[8192];
void wine_test_visual(void);
void wine_test_device(void);
void wine_test_stateblock(void);
void wine_test_d3d9ex(void);
unsigned wine_case_count(void) { return ARRAY_SIZE(wine_cases); }
const struct WineCase* wine_case_at(unsigned index) { return index<ARRAY_SIZE(wine_cases)?&wine_cases[index]:NULL; }
void wine_location(const char* file,unsigned row) { source=file;line=row; }
void wine_ok(int condition,const char* format,...) {
    if(condition) { ++passed;return; }
    ++failed;if(todo)++todo_failed;
    va_list ap;va_start(ap,format);vsnprintf(message,sizeof message,format,ap);va_end(ap);
    fprintf(stderr,"FAIL %s:%u %s%s",source,line,todo?"[upstream TODO] ":"",message);fflush(stderr);
    /* Stop at the first failed assertion; the next case runs in a new process.
       This protects against follow-on NULL dereferences after failed API calls. */
    longjmp(escape_case,1);
}
void wine_skip(const char* format,...) {
    ++skipped;va_list ap;va_start(ap,format);vsnprintf(message,sizeof message,format,ap);va_end(ap);
    fprintf(stderr,"SKIP %s:%u %s",source,line,message);fflush(stderr);
}
void wine_platform_skip(const char* reason) {
    platform_skip=1;snprintf(message,sizeof message,"%s",reason);longjmp(escape_case,1);
}
void wine_trace(const char* format,...) {
    va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);
}
void wine_todo_begin(int active) { todo=active;todo_pending=1; }
int wine_todo_next(void) { if(todo_pending) { todo_pending=0;return 1; } todo=0;return 0; }
void winetest_push_context(const char* format,...) { va_list ap;va_start(ap,format);vfprintf(stderr,format,ap);va_end(ap);fputc('\n',stderr); }
void winetest_pop_context(void) {}
const char* wine_dbgstr_rect(const RECT* r) {
    static char b[128];if(!r)return "(null)";
    snprintf(b,sizeof b,"(%d,%d)-(%d,%d)",r->left,r->top,r->right,r->bottom);return b;
}
void wine_run_case(const char* name,void(*function)(void)) {
    if(strcmp(name,selected))return;entered=1;function();
}
static void json_string(FILE* f,const char* s) {
    fputc('"',f);for(;*s;++s) { unsigned char c=*s;if(c=='"'||c=='\\')fprintf(f,"\\%c",c);
      else if(c<32)fprintf(f,"\\u%04x",c);else fputc(c,f); }fputc('"',f);
}
int wine_execute(unsigned index,FILE* report) {
    const struct WineCase* test=wine_case_at(index);if(!test)return 2;
    selected=test->name;passed=failed=skipped=todo_failed=0;todo=entered=platform_skip=0;
    source="module initialization";line=0;message[0]=0;
    fprintf(stderr,"RUN %u %s\n",index,selected);fflush(stderr);
    if(!setjmp(escape_case)) {
        if(*test->platform_block)wine_platform_skip(test->platform_block);
        if(!strcmp(test->module,"visual"))wine_test_visual();
        else if(!strcmp(test->module,"device"))wine_test_device();
        else if(!strcmp(test->module,"stateblock"))wine_test_stateblock();
        else wine_test_d3d9ex();
    }
    const char* status=failed?"fail":platform_skip?"platform-skip":!entered?"setup-skip":skipped?"partial-skip":"pass";
    fprintf(report,"{\"index\":%u,\"name\":",index);json_string(report,selected);
    fprintf(report,",\"status\":");json_string(report,status);
    fprintf(report,",\"passed_assertions\":%u,\"failed_assertions\":%u,\"todo_failures\":%u,\"skips\":%u,\"source\":",passed,failed,todo_failed,skipped);
    json_string(report,source);fprintf(report,",\"line\":%u,\"message\":",line);json_string(report,message);fprintf(report,"}\n");fflush(report);
    fprintf(stderr,"END %s %s (%u assertions passed)\n",selected,status,passed);fflush(stderr);
    return failed?1:0;
}
