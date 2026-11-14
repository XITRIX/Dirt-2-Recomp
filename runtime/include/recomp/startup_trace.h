#pragma once
// Durable startup checkpoints: direct writes avoid stdio allocation while the
// virtual-memory manager is locked. Only Switch 2 bring-up uses this file.
#if defined(__SWITCH2__)
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
namespace rt {
inline void startup_trace(const char* stage, uint64_t value=0, uint64_t bytes=0, uint32_t result=0) {
    static const int fd=[] {
        ::rename("sdmc:/switch/dirt2/startup-stage.log", "sdmc:/switch/dirt2/startup-stage.prev.log");
        return ::open("sdmc:/switch/dirt2/startup-stage.log", O_WRONLY|O_CREAT|O_TRUNC, 0666);
    }();
    if(fd<0) return;
    char line[240];
    const int count=std::snprintf(line,sizeof line,"%s value=0x%llX bytes=0x%llX result=0x%X\n",stage,
        (unsigned long long)value,(unsigned long long)bytes,unsigned(result));
    if(count<=0 || count>=int(sizeof line)) return;
    for(int at=0;at<count;) {
        const auto written=::write(fd,line+at,count-at);
        if(written<=0) break;
        at+=int(written);
    }
    ::fsync(fd);
}
}
#else
namespace rt { inline void startup_trace(const char*, unsigned long long=0, unsigned long long=0, unsigned=0) {} }
#endif
