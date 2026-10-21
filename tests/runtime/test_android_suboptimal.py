#!/usr/bin/env python3
"""Compile actual Android presenter policy with mocked surface capabilities.

Run after tools/scripts/prepare_android_dxvk.py; optionally pass another
prepared dxvk_presenter.cpp path. Requires a host C++17 compiler (CXX or c++).
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'build/src/dxvk-android/src/dxvk/dxvk_presenter.cpp'
src = source.read_text()
helper=src[src.index('  bool Presenter::suboptimalNeedsRecreation()'):src.index('\n#endif',src.index('  bool Presenter::suboptimalNeedsRecreation()'))]
pick=src[src.index('  VkExtent2D Presenter::pickImageExtent('):src.index('  uint32_t Presenter::pickImageCount(')]
acquire=re.search(r'    auto acceptSuboptimalAcquire = \[this\] \{.*?    };',src,re.S).group(0)
present = re.search(r'    if \(status == VK_SUBOPTIMAL_KHR\) \{.*?\n    \}', src, re.S).group(0)

code=r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>
#include <iostream>
using std::clamp;
using VkResult=int;
constexpr int VK_SUCCESS=0, VK_SUBOPTIMAL_KHR=1, VK_ERROR_SURFACE_LOST_KHR=-1, VK_ERROR_OUT_OF_DATE_KHR=-2;
struct VkExtent2D {uint32_t width,height;};
struct VkSurfaceCapabilitiesKHR {VkExtent2D currentExtent,minImageExtent,maxImageExtent;};
struct Image {struct Info {VkExtent2D extent;} value; const Info& info(){return value;}};
struct Device {Device* adapter(){return this;} int handle(){return 0;}};
static bool surfaceLocked = false;
struct CheckedMutex {
 void lock(){assert(!surfaceLocked);surfaceLocked=true;}
 void unlock(){assert(surfaceLocked);surfaceLocked=false;}
};
struct Api {int result=0; VkSurfaceCapabilitiesKHR caps{{3200,2136},{1,1},{4096,4096}}; int vkGetPhysicalDeviceSurfaceCapabilitiesKHR(int,int,VkSurfaceCapabilitiesKHR* out){assert(surfaceLocked);*out=caps;return result;}};
struct Presenter {
 CheckedMutex m_surfaceMutex;
 Api api; Api* m_vki=&api; Device device; Device* m_device=&device; int m_surface=0;
 Image image{{{3200,2136}}}; std::vector<Image*> m_images{&image}; VkExtent2D m_preferredExtent{3200,2136};
 int m_acquireStatus=0; bool m_dirtySwapchain=false;
 bool suboptimalNeedsRecreation(); VkExtent2D pickImageExtent(const VkSurfaceCapabilitiesKHR&,VkExtent2D);
 bool checkedNeedsRecreation(){std::lock_guard lock(m_surfaceMutex);return suboptimalNeedsRecreation();}
 void acquire(){std::lock_guard lock(m_surfaceMutex);ACQUIRE acceptSuboptimalAcquire();}
 int present(int status){
PRESENT
    return status;
 }
};
HELPER
PICK
int main(){
 Presenter p;
 assert(!p.checkedNeedsRecreation());
 assert(p.present(VK_SUBOPTIMAL_KHR)==VK_SUCCESS);
 p.m_acquireStatus=VK_SUBOPTIMAL_KHR;p.acquire();assert(p.m_acquireStatus==VK_SUCCESS && !p.m_dirtySwapchain);
 p.api.caps.currentExtent={2136,3200};assert(p.checkedNeedsRecreation());
 assert(p.present(VK_SUBOPTIMAL_KHR)==VK_SUBOPTIMAL_KHR);
 p.m_acquireStatus=VK_SUBOPTIMAL_KHR;p.acquire();assert(p.m_acquireStatus==VK_SUCCESS && p.m_dirtySwapchain);
 p.api.caps.currentExtent={0,0};assert(p.checkedNeedsRecreation());
 p.api.caps.currentExtent={UINT32_MAX,UINT32_MAX};assert(!p.checkedNeedsRecreation());
 p.api.caps.maxImageExtent={1600,1200};assert(p.checkedNeedsRecreation());
 p.api.result=VK_ERROR_SURFACE_LOST_KHR;assert(p.checkedNeedsRecreation());
 assert(p.present(VK_ERROR_SURFACE_LOST_KHR)==VK_ERROR_SURFACE_LOST_KHR);
 p.m_acquireStatus=VK_ERROR_OUT_OF_DATE_KHR;p.acquire();assert(p.m_acquireStatus==VK_ERROR_OUT_OF_DATE_KHR);
 p.api.result=0;p.m_images.clear();assert(p.checkedNeedsRecreation());
 std::cout<<"PASS: stable/changed/zero/clamped extents; query failure; acquired-image deferral; error preservation; surface query locking\n";
}
'''.replace('ACQUIRE',acquire).replace('PRESENT',present).replace('HELPER',helper).replace('PICK',pick)
with tempfile.TemporaryDirectory(prefix="dirt2-suboptimal-") as tmp:
    source_path = Path(tmp) / "test.cpp"
    executable = Path(tmp) / "test"
    source_path.write_text(code)
    subprocess.run(shlex.split(os.environ.get("CXX", "c++")) + [
        "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source_path),
        "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
