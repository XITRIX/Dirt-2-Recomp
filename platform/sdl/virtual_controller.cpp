#include "virtual_controller.h"
#include "touch_layout.h"
#include <mutex>
#include <string_view>

namespace platform::touch {
#ifdef __ANDROID__
bool android_keyboard_or_mouse();
#endif
namespace {
std::mutex mutex;
Layout layout;
SDL_Window* window=nullptr;
bool visible=false, focused=true;
SDL_Rect lastSafe{};
void cancel() { layout.contacts.clear(); }
// Enumerate real devices. SDL's touch/pen mouse events and on-screen keyboard
// are synthetic and must never suppress the controller.
bool hardware() {
#ifdef __ANDROID__
    if(android_keyboard_or_mouse()) return true;
#endif
    int count=0;
    auto* pads=SDL_GetGamepads(&count); SDL_free(pads);
    if(count>0 || SDL_HasKeyboard()) return true;
    auto* mice=SDL_GetMice(&count);
    bool physical=false;
    for(int i=0;i<count;++i)
        if(mice[i]!=SDL_TOUCH_MOUSEID && mice[i]!=SDL_PEN_MOUSEID) physical=true;
    SDL_free(mice);
    return physical;
}
uint32_t rgba(unsigned r,unsigned g,unsigned b,unsigned a) { return r | g<<8 | b<<16 | a<<24; }
constexpr uint32_t white=0xc7ffffff, dark=0x90000000;
void tri(Frame& f, float ax,float ay,float bx,float by,float cx,float cy,uint32_t c) {
    f.vertices.insert(f.vertices.end(),{{ax,ay,c},{bx,by,c},{cx,cy,c}});
}
void box(Frame& f,Rect r,uint32_t c) {
    tri(f,r.x,r.y,r.x+r.w,r.y,r.x+r.w,r.y+r.h,c);
    tri(f,r.x,r.y,r.x+r.w,r.y+r.h,r.x,r.y+r.h,c);
}
void rounded(Frame& f,Rect r,uint32_t c) {
    float radius=std::min(r.w,r.h)*.3f;
    box(f,{r.x+radius,r.y,r.w-2*radius,r.h},c);
    box(f,{r.x,r.y+radius,radius,r.h-2*radius},c);
    box(f,{r.x+r.w-radius,r.y+radius,radius,r.h-2*radius},c);
    for(int corner=0;corner<4;++corner) {
        float cx=corner==0||corner==3?r.x+radius:r.x+r.w-radius;
        float cy=corner<2?r.y+radius:r.y+r.h-radius;
        for(int i=0;i<12;++i) {
            float a=(corner*12+i)*6.2831853f/48+3.14159265f;
            float b=a+6.2831853f/48;
            tri(f,cx,cy,cx+std::cos(a)*radius,cy+std::sin(a)*radius,cx+std::cos(b)*radius,cy+std::sin(b)*radius,c);
        }
    }
}
void shape(Frame& f,Rect r,bool circle,uint32_t color,uint32_t fill) {
    // Separate fill and border triangles preserve translucency: drawing a solid
    // border under a translucent fill would make the entire control opaque.
    std::array<std::pair<float,float>,48> outer,inner;
    for(int i=0;i<48;++i) {
        float angle=float(i)*6.2831853f/48;
        if(circle) {
            outer[i]={r.cx()+std::cos(angle)*r.w/2,r.cy()+std::sin(angle)*r.h/2};
            inner[i]={r.cx()+std::cos(angle)*(r.w/2-1.5f),r.cy()+std::sin(angle)*(r.h/2-1.5f)};
        } else {
            int corner=i/12;
            float radius=std::min(r.w,r.h)*.3f;
            float cx=corner==0||corner==3?r.x+radius:r.x+r.w-radius;
            float cy=corner<2?r.y+radius:r.y+r.h-radius;
            angle=3.14159265f+corner*1.5707963f+(i%12)*1.5707963f/11;
            outer[i]={cx+std::cos(angle)*radius,cy+std::sin(angle)*radius};
            inner[i]={cx+std::cos(angle)*(radius-1.5f),cy+std::sin(angle)*(radius-1.5f)};
        }
    }
    for(int i=0;i<48;++i) {
        auto [ax,ay]=outer[i]; auto [bx,by]=outer[(i+1)%48];
        auto [cx,cy]=inner[i]; auto [dx,dy]=inner[(i+1)%48];
        tri(f,r.cx(),r.cy(),cx,cy,dx,dy,fill);
        tri(f,ax,ay,bx,by,cx,cy,color); tri(f,cx,cy,bx,by,dx,dy,color);
    }
}
// Small built-in vector-pixel alphabet keeps labels independent of platform fonts.
std::array<uint8_t,7> glyph(char c) {
    switch(c) {
    case 'A': return {14,17,17,31,17,17,17}; case 'B': return {30,17,17,30,17,17,30};
    case 'C': return {14,17,16,16,16,17,14}; case 'K': return {17,18,20,24,20,18,17};
    case 'L': return {16,16,16,16,16,16,31}; case 'R': return {30,17,17,30,20,18,17};
    case 'S': return {15,16,16,14,1,1,30}; case 'T': return {31,4,4,4,4,4,4};
    case 'X': return {17,17,10,4,10,17,17}; case 'Y': return {17,17,10,4,4,4,4};
    case '3': return {30,1,1,14,1,1,30}; default: return {};
    }
}
void label(Frame& f,Rect r,std::string_view text,uint32_t color=white) {
    float pixel=std::min(r.h*.45f/7,r.w*.8f/(text.size()*6-1));
    float x=r.cx()-(text.size()*6-1)*pixel/2, y=r.cy()-7*pixel/2;
    for(char c:text) {
        auto rows=glyph(c);
        for(int j=0;j<7;++j) for(int i=0;i<5;++i)
            if(rows[j]&(16>>i)) box(f,{x+i*pixel,y+j*pixel,pixel,pixel},color);
        x+=6*pixel;
    }
}
}
void attach(SDL_Window* w) { std::lock_guard lock(mutex); window=w; focused=true; visible=false; layout.width=0; cancel(); }
void detach(SDL_Window* w) { std::lock_guard lock(mutex); if(window==w) { window=nullptr; visible=false; cancel(); } }
void update() {
    std::lock_guard lock(mutex);
    if(!window) return;
    int w=0,h=0; SDL_GetWindowSize(window,&w,&h);
    SDL_Rect safe{0,0,w,h}; SDL_GetWindowSafeArea(window,&safe);
    if(w!=layout.width || h!=layout.height || safe.x!=lastSafe.x || safe.y!=lastSafe.y || safe.w!=lastSafe.w || safe.h!=lastSafe.h) {
        layout.resize(float(w),float(h),{float(safe.x),float(safe.y),float(safe.w),float(safe.h)}); lastSafe=safe;
    }
    bool next=focused && w>0 && h>0 && !hardware();
    if(next!=visible) { cancel(); visible=next; }
}
void event(const SDL_Event& e) {
    std::lock_guard lock(mutex);
    if(!window) return;
    const auto wid=SDL_GetWindowID(window);
    if(e.type==SDL_EVENT_WILL_ENTER_BACKGROUND || (e.type==SDL_EVENT_WINDOW_FOCUS_LOST && e.window.windowID==wid)) {
        focused=false; visible=false; cancel();
    }
    if(e.type==SDL_EVENT_DID_ENTER_FOREGROUND || (e.type==SDL_EVENT_WINDOW_FOCUS_GAINED && e.window.windowID==wid)) focused=true;
    // Device hotplug cancels held input immediately, before the next poll.
    if(e.type==SDL_EVENT_GAMEPAD_ADDED || e.type==SDL_EVENT_KEYBOARD_ADDED || e.type==SDL_EVENT_MOUSE_ADDED) {
        if(hardware()) { visible=false; cancel(); }
    }
    if(!visible) return;
    if(e.type==SDL_EVENT_FINGER_DOWN || e.type==SDL_EVENT_FINGER_MOTION || e.type==SDL_EVENT_FINGER_UP || e.type==SDL_EVENT_FINGER_CANCELED) {
        if(e.tfinger.windowID!=wid || e.tfinger.touchID==SDL_MOUSE_TOUCHID) return;
        float x=e.tfinger.x*layout.width,y=e.tfinger.y*layout.height;
        if(e.type==SDL_EVENT_FINGER_DOWN) layout.down(e.tfinger.touchID,e.tfinger.fingerID,x,y);
        else if(e.type==SDL_EVENT_FINGER_MOTION) layout.move(e.tfinger.touchID,e.tfinger.fingerID,x,y);
        else layout.contacts.erase({e.tfinger.touchID,e.tfinger.fingerID});
    }
}
State state() { std::lock_guard lock(mutex); return visible?layout.snapshot<State>():State{}; }
Frame frame() {
    std::lock_guard lock(mutex);
    Frame f;
    if(!visible) return f;
    f.width=layout.width; f.height=layout.height;
    f.vertices.reserve(12000);
    auto state=layout.snapshot<State>();
    constexpr const char* labels[]={"LT","LB","RB","RT","Y","B","A","X","L3","R3","BACK","START"};
    constexpr uint16_t masks[]={0,0x100,0x200,0,0x8000,0x2000,0x1000,0x4000,0x40,0x80,0x20,0x10};
    for(int i=0;i<Dpad;++i) {
        uint32_t color=white;
        if(i==A) color=rgba(48,209,88,220); if(i==B) color=rgba(255,69,58,220);
        if(i==X) color=rgba(10,132,255,220); if(i==Y) color=rgba(255,214,10,220);
        bool pressed=i==LT?state.lt:i==RT?state.rt:state.buttons&masks[i];
        shape(f,layout.rects[i],i>=Y&&i<=X,color,pressed?rgba(color&255,(color>>8)&255,(color>>16)&255,80):dark);
        label(f,layout.rects[i],labels[i],color);
    }
    auto r=layout.rects[Dpad]; float third=r.w/3;
    box(f,{r.x+third,r.y,third,r.h},dark);
    box(f,{r.x,r.y+third,third,third},dark); box(f,{r.x+2*third,r.y+third,third,third},dark);
    const std::pair<float,float> outline[]={{1,0},{2,0},{2,1},{3,1},{3,2},{2,2},{2,3},{1,3},{1,2},{0,2},{0,1},{1,1}};
    for(int i=0;i<12;++i) {
        auto [ax,ay]=outline[i]; auto [bx,by]=outline[(i+1)%12];
        box(f,{r.x+std::min(ax,bx)*third-.6f,r.y+std::min(ay,by)*third-.6f,
            std::abs(ax-bx)*third+1.2f,std::abs(ay-by)*third+1.2f},white);
    }
    const Rect segments[]={{r.x+third,r.y,third,third},{r.x+third,r.y+2*third,third,third},
        {r.x,r.y+third,third,third},{r.x+2*third,r.y+third,third,third}};
    for(int i=0;i<4;++i) {
        auto s=segments[i]; if(state.buttons&(1<<i)) rounded(f,s,0x50ffffff);
        float x=s.cx(),y=s.cy(),d=third*.16f;
        if(i==0) tri(f,x,y-d,x+d,y+d,x-d,y+d,white);
        if(i==1) tri(f,x,y+d,x-d,y-d,x+d,y-d,white);
        if(i==2) tri(f,x-d,y,x+d,y-d,x+d,y+d,white);
        if(i==3) tri(f,x+d,y,x-d,y+d,x-d,y-d,white);
    }
    for(int i=LeftStick;i<=RightStick;++i) {
        r=layout.rects[i]; shape(f,r,true,white,dark);
        float x=(i==LeftStick?state.lx:state.rx)/32767.f,y=(i==LeftStick?state.ly:state.ry)/32767.f;
        Rect knob{r.cx()+x*r.w*.23f-r.w*.24f,r.cy()-y*r.w*.23f-r.w*.24f,r.w*.48f,r.w*.48f};
        shape(f,knob,true,white,0xc2000000); label(f,knob,i==LeftStick?"L":"R");
    }
    return f;
}
}
