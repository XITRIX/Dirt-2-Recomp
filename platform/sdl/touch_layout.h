// Portable counterpart of RPCSX-iOS/RPCS3/UI/Session/PS3TouchControllerView.swift.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>

namespace platform::touch {
enum Control { LT, LB, RB, RT, Y, B, A, X, LS, RS, Back, Start, Dpad, LeftStick, RightStick, Count };
struct Rect {
    float x=0, y=0, w=0, h=0;
    float cx() const { return x+w/2; }
    float cy() const { return y+h/2; }
    bool contains(float px, float py, float extra=0) const {
        return px>=x-extra && px<=x+w+extra && py>=y-extra && py<=y+h+extra;
    }
};
struct Contact { Control control; float x, y; };
class Layout {
public:
    std::array<Rect, Count> rects{};
    // SDL touch and finger IDs together identify a contact across devices.
    std::map<std::pair<uint64_t,uint64_t>, Contact> contacts;
    float width=0, height=0;
    void resize(float w, float h, Rect safe) {
        contacts.clear(); width=w; height=h;
        // Use points on HiDPI devices, matching UIKit's sizing in the reference.
        const float inset=14, bottom=std::max(18.f, h-safe.y-safe.h+12);
        const float left=safe.x+inset, right=w-safe.x-safe.w+inset;
        const float gap=std::max(88.f,std::min(std::clamp(w*.22f,132.f,220.f),w-left-right-192));
        const float cluster=std::min(320.f,(w-left-right-gap)*.5f);
        const float dp=std::clamp(std::min(h*.32f,cluster*.47f),80.f,132.f);
        const float stick=std::clamp(std::min(h*.25f,cluster*.36f),64.f,104.f);
        const float face=std::clamp(dp*.34f,34.f,46.f);
        const float offset=(face+std::max(10.f,face*.24f))/std::sqrt(2.f), span=offset*2+face;
        const float sh=cluster<160?32:36, sw=std::min(80.f,(cluster-8)/2);
        const float upper=std::max(dp,span), stack=28+8+stick;
        const float body=upper+stick+8>cluster?upper+14+stack:std::max(upper+18,stack);
        const float base=h-bottom, top=base-sh-14-body, controls=top+sh+14;
        const float rx=w-right-cluster;
        rects[LT]={left,top,sw,sh}; rects[LB]={left+sw+8,top,sw,sh};
        rects[RT]={w-right-sw,top,sw,sh}; rects[RB]={w-right-sw*2-8,top,sw,sh};
        rects[Dpad]={left,controls,dp,dp};
        auto center=[&](Control c,float x,float y,float a,float b) { rects[c]={x-a/2,y-b/2,a,b}; };
        const float fx=w-right-span/2, fy=controls+span/2;
        center(Y,fx,fy-offset,face,face); center(B,fx+offset,fy,face,face);
        center(A,fx,fy+offset,face,face); center(X,fx-offset,fy,face,face);
        rects[LeftStick]={left+cluster-stick,base-stick,stick,stick};
        rects[RightStick]={rx,base-stick,stick,stick};
        center(LS,rects[LeftStick].cx(),base-stick-8-14,std::min(48.f,stick),28);
        center(RS,rects[RightStick].cx(),base-stick-8-14,std::min(48.f,stick),28);
        // XInput has no PS button: retain the reference's center gap.
        const float meta=std::min(gap<180?40.f:62.f,(gap-44-16)/2);
        center(Back,w/2-30-meta/2,base-15,meta,30);
        center(Start,w/2+30+meta/2,base-15,meta,30);
    }
    int hit(float x,float y,bool buttonsOnly=false) const {
        const int end=buttonsOnly?Dpad:Count;
        for(int i=0;i<end;++i) if(rects[i].contains(x,y)) return i;
        int best=-1; float distance=1e30f;
        for(int i=0;i<end;++i) if(rects[i].contains(x,y,i<Dpad?8:10)) {
            float dx=x-rects[i].cx(),dy=y-rects[i].cy(),d=dx*dx+dy*dy;
            if(d<distance) { best=i; distance=d; }
        }
        return best;
    }
    bool occupied(int c, std::pair<uint64_t,uint64_t> except) const {
        for(auto& [id,t]:contacts) if(id!=except && t.control==c) return true;
        return false;
    }
    void down(uint64_t device,uint64_t finger,float x,float y) {
        const auto id=std::make_pair(device,finger); int c=hit(x,y);
        if(c>=0 && !occupied(c,id)) contacts.insert_or_assign(id,Contact{Control(c),x,y});
    }
    void move(uint64_t device,uint64_t finger,float x,float y) {
        const auto id=std::make_pair(device,finger); auto it=contacts.find(id);
        if(it==contacts.end()) return;
        int c=hit(x,y,true);
        if(it->second.control<Dpad && c>=0 && !occupied(c,id)) it->second.control=Control(c);
        it->second.x=x; it->second.y=y;
    }
    std::pair<float,float> stick(const Contact& t) const {
        auto r=rects[t.control]; float x=(t.x-r.cx())/(r.w/2),y=(r.cy()-t.y)/(r.h/2);
        float length=std::max(1.f,std::hypot(x,y)); return {x/length,y/length};
    }
    template<class State> State snapshot() const {
        State s;
        constexpr uint16_t masks[]={0,0x100,0x200,0,0x8000,0x2000,0x1000,0x4000,0x40,0x80,0x20,0x10};
        for(auto& [id,t]:contacts) {
            if(t.control==Dpad) {
                auto r=rects[Dpad]; float x=(t.x-r.cx())/(r.w/2),y=(r.cy()-t.y)/(r.h/2);
                if(std::hypot(x,y)<.2f) continue;
                if(y>.28f) s.buttons|=1; if(y<-.28f) s.buttons|=2;
                if(x<-.28f) s.buttons|=4; if(x>.28f) s.buttons|=8;
            } else if(t.control>=LeftStick) {
                auto [x,y]=stick(t);
                if(t.control==LeftStick) { s.lx=int16_t(x*32767); s.ly=int16_t(y*32767); }
                else { s.rx=int16_t(x*32767); s.ry=int16_t(y*32767); }
            } else if(rects[t.control].contains(t.x,t.y,8)) {
                if(t.control==LT) s.lt=255; else if(t.control==RT) s.rt=255;
                else s.buttons|=masks[t.control];
            }
        }
        return s;
    }
};
}
