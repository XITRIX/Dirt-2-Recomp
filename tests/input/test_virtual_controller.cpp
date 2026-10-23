// Deterministic SDL boundary tests: no display, GPU or physical devices needed.
// c++ -std=c++20 -DDIRT2_TOUCH_CONTROLLER=1 -I. -Ibuild/deps/macos/include \
//   tests/input/test_virtual_controller.cpp platform/sdl/virtual_controller.cpp -o /tmp/test_touch
#include "platform/sdl/virtual_controller.h"
#include "platform/sdl/touch_layout.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
using namespace platform::touch;
namespace {
bool keyboard=false;
int pads=0;
std::vector<SDL_MouseID> mice;
int width=852,height=393;
SDL_Rect safe{59,0,734,372};
SDL_Window* fakeWindow=reinterpret_cast<SDL_Window*>(1);
}
extern "C" {
SDL_JoystickID* SDL_GetGamepads(int* count) { *count=pads; return nullptr; }
bool SDL_HasKeyboard() { return keyboard; }
SDL_MouseID* SDL_GetMice(int* count) { *count=int(mice.size()); return mice.data(); }
void SDL_free(void*) {}
bool SDL_GetWindowSize(SDL_Window*,int* w,int* h) { *w=width; *h=height; return true; }
bool SDL_GetWindowSafeArea(SDL_Window*,SDL_Rect* r) { *r=safe; return true; }
SDL_WindowID SDL_GetWindowID(SDL_Window*) { return 1; }
}
void finger(Uint32 type,int id,float x,float y,SDL_WindowID window=1,SDL_TouchID device=7) {
    SDL_Event e{}; e.type=type; e.tfinger.windowID=window; e.tfinger.touchID=device;
    e.tfinger.fingerID=id; e.tfinger.x=x/width; e.tfinger.y=y/height; event(e);
}
void press(Layout& l,Control c,int id) { auto r=l.rects[c]; finger(SDL_EVENT_FINGER_DOWN,id,r.cx(),r.cy()); }
void lifecycle(Uint32 type) { SDL_Event e{}; e.type=type; e.window.windowID=1; event(e); }
bool neutral() { auto s=state(); return !s.buttons&&!s.lt&&!s.rt&&!s.lx&&!s.ly&&!s.rx&&!s.ry; }
int main(int argc,char** argv) {
    Layout l; l.resize(width,height,{float(safe.x),float(safe.y),float(safe.w),float(safe.h)});
    attach(fakeWindow); update(); assert(!frame().vertices.empty());
    // Every digital button and trigger uses the game's XInput mapping.
    const uint16_t masks[]={0,0x100,0x200,0,0x8000,0x2000,0x1000,0x4000,0x40,0x80,0x20,0x10};
    for(int i=0;i<Dpad;++i) {
        press(l,Control(i),50);
        auto s=state();
        assert(s.buttons==masks[i] && s.lt==(i==LT?255:0) && s.rt==(i==RT?255:0));
        finger(SDL_EVENT_FINGER_UP,50,0,0); assert(neutral());
    }
    // A second finger cannot steal a held control or release the first finger.
    press(l,A,1); press(l,A,2); finger(SDL_EVENT_FINGER_UP,2,0,0);
    assert(state().buttons==0x1000); finger(SDL_EVENT_FINGER_UP,1,0,0);
    // Throttle, steering, face button and right stick work simultaneously.
    press(l,RT,1); press(l,A,2);
    auto left=l.rects[LeftStick],right=l.rects[RightStick];
    finger(SDL_EVENT_FINGER_DOWN,3,left.cx(),left.cy());
    finger(SDL_EVENT_FINGER_MOTION,3,left.cx()-left.w,left.cy()-left.h);
    finger(SDL_EVENT_FINGER_DOWN,4,right.cx(),right.cy());
    finger(SDL_EVENT_FINGER_MOTION,4,right.cx()+right.w,right.cy());
    auto s=state(); assert(s.rt==255 && (s.buttons&0x1000) && s.lx<0 && s.ly>0 && s.rx==32767);
    assert(std::abs(std::hypot(s.lx,s.ly)-32767)<2);
    // Sliding buttons transfers ownership; leaving a button releases it.
    auto b=l.rects[B]; finger(SDL_EVENT_FINGER_MOTION,2,b.cx(),b.cy());
    assert((state().buttons&0x2000) && !(state().buttons&0x1000));
    finger(SDL_EVENT_FINGER_MOTION,2,width/2,20); assert(!state().buttons);
    finger(SDL_EVENT_FINGER_CANCELED,1,0,0); assert(!state().rt);
    finger(SDL_EVENT_FINGER_UP,3,0,0); finger(SDL_EVENT_FINGER_UP,4,0,0); assert(neutral());
    // D-pad diagonals and center dead zone.
    auto dp=l.rects[Dpad]; finger(SDL_EVENT_FINGER_DOWN,5,dp.x,dp.y); assert((state().buttons&15)==5);
    finger(SDL_EVENT_FINGER_MOTION,5,dp.cx(),dp.cy()); assert(neutral());
    finger(SDL_EVENT_FINGER_UP,5,0,0);
    // Identical finger IDs on different devices must not collide.
    auto a=l.rects[A],rt=l.rects[RT];
    finger(SDL_EVENT_FINGER_DOWN,1,a.cx(),a.cy(),1,7);
    finger(SDL_EVENT_FINGER_DOWN,1,rt.cx(),rt.cy(),1,8);
    finger(SDL_EVENT_FINGER_UP,1,0,0,1,7); assert(state().rt==255 && !state().buttons);
    finger(SDL_EVENT_FINGER_UP,1,0,0,1,8);
    // Synthetic mouse touches and touches for another window are ignored.
    finger(SDL_EVENT_FINGER_DOWN,1,a.cx(),a.cy(),1,SDL_MOUSE_TOUCHID);
    finger(SDL_EVENT_FINGER_DOWN,2,a.cx(),a.cy(),2); assert(neutral());
    // Connecting any physical input immediately hides/neutralizes the pad.
    for(int device=0;device<3;++device) {
        press(l,RT,1);
        if(device==0) pads=1; if(device==1) keyboard=true; if(device==2) mice={123};
        lifecycle(device==0?SDL_EVENT_GAMEPAD_ADDED:device==1?SDL_EVENT_KEYBOARD_ADDED:SDL_EVENT_MOUSE_ADDED);
        assert(frame().vertices.empty() && neutral());
        pads=0; keyboard=false; mice.clear(); update(); assert(!frame().vertices.empty() && neutral());
        finger(SDL_EVENT_FINGER_MOTION,1,rt.cx(),rt.cy()); assert(neutral());
    }
    mice={SDL_TOUCH_MOUSEID,SDL_PEN_MOUSEID}; update(); assert(!frame().vertices.empty()); mice.clear();
    // Focus/background and resize discard held contacts, including late ups.
    for(auto type:{SDL_EVENT_WINDOW_FOCUS_LOST,SDL_EVENT_WILL_ENTER_BACKGROUND}) {
        press(l,RT,1); lifecycle(type); assert(neutral() && frame().vertices.empty());
        update(); assert(frame().vertices.empty());
        lifecycle(type==SDL_EVENT_WINDOW_FOCUS_LOST?SDL_EVENT_WINDOW_FOCUS_GAINED:SDL_EVENT_DID_ENTER_FOREGROUND);
        update(); assert(neutral() && !frame().vertices.empty());
    }
    press(l,RT,1); safe.x+=1; update(); assert(neutral());
    // Every control remains on screen and hittable at representative sizes.
    for(auto [w,h]:{std::pair{667,375},std::pair{852,393},std::pair{1024,768},std::pair{1920,1080},std::pair{393,852}}) {
        Layout size; size.resize(w,h,{0,0,float(w),float(h)});
        for(int i=0;i<Count;++i) {
            auto r=size.rects[i]; assert(r.x>=0 && r.y>=0 && r.x+r.w<=w && r.y+r.h<=h);
            assert(size.hit(r.cx(),r.cy())==i);
        }
    }
    if(argc>1) {
        auto f=frame(); std::ofstream out(argv[1]);
        out<<"{\"width\":"<<f.width<<",\"height\":"<<f.height<<",\"vertices\":[";
        bool first=true; for(auto v:f.vertices) { if(!first) out<<','; first=false; out<<'['<<v.x<<','<<v.y<<','<<v.color<<']'; } out<<"]}";
    }
    detach(fakeWindow); assert(frame().vertices.empty() && neutral());
    std::cout<<"virtual controller tests passed\n";
}
