#!/usr/bin/env python3
"""Exercise shared COM/private ownership code, including parent cascade deletion."""
from pathlib import Path
import tempfile,subprocess,os
root=Path(__file__).resolve().parents[2]
source=r'''
#include <cassert>
#include <vector>
struct IUnknown{virtual unsigned AddRef()=0;virtual unsigned Release()=0;virtual ~IUnknown()=default;};
'''+ '#include "'+str(root/'shims/d3d9/deko/object_refs.h')+'"\n'+r'''
unsigned devices=0,resources=0;
struct Resource;
struct Device:IUnknown{
 deko::ObjectRefs refs;std::vector<Resource*>bound;
 Device(){++devices;}~Device();
 unsigned AddRef()override{return refs.add_ref();}unsigned Release()override{return refs.release(this);}
};
struct Resource:IUnknown{
 deko::ObjectRefs refs;Resource(Device*d){++resources;refs.set_parent(d);}~Resource(){--resources;}
 unsigned AddRef()override{return refs.add_ref();}unsigned Release()override{return refs.release(this);}
 void bind(){refs.add_private();}void unbind(){refs.release_private(this);}
};
Device::~Device(){for(auto*r:bound)r->unbind();--devices;}
int main(){
 // Bound resources remain alive at public count zero without keeping device alive.
 auto*d=new Device;auto*r=new Resource(d);r->bind();d->bound.push_back(r);
 assert(r->AddRef()==2);assert(r->Release()==1);assert(r->Release()==0);
 assert(resources==1&&devices==1);assert(d->Release()==0);assert(resources==0&&devices==0);
 // Public resource refs keep the device alive after its caller releases it.
 d=new Device;r=new Resource(d);assert(d->Release()==1);assert(r->Release()==0);assert(!resources&&!devices);
 // A bound object can be resurrected by GetXXX. Parent destruction may delete
 // that same object during the final public Release, without accessing freed data.
 d=new Device;r=new Resource(d);r->bind();d->bound.push_back(r);assert(r->Release()==0);
 assert(r->AddRef()==1);assert(d->Release()==1);assert(r->Release()==0);assert(!resources&&!devices);
 // Implicit surfaces begin with only private ownership, expose zero-based COM count.
 d=new Device;r=new Resource(d);r->bind();d->bound.push_back(r);assert(r->Release()==0);
 assert(r->AddRef()==1);assert(r->Release()==0);assert(d->Release()==0);assert(!resources&&!devices);
}
'''
with tempfile.TemporaryDirectory(prefix='d3d9-object-refs-') as directory:
 p=Path(directory);(p/'test.cpp').write_text(source)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('ObjectRefs actual implementation: public/private counts, implicit refs, resurrection, live-child retention and cascading parent destruction passed (ASAN/UBSAN)')
