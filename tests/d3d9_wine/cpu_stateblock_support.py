"""Shared declarations for existing isolated method tests (recording disabled).

The dedicated state-block test supplies a full device and exercises recording.
These older fixtures only need the actual entry storage/template declarations.
"""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def declarations():
    text=(ROOT/'shims/d3d9/deko/stateblock.h').read_text()
    text=text.replace('#pragma once','').replace('namespace deko {','').replace('} // namespace deko','')
    base='''
#ifndef STDMETHOD
#define STDMETHOD(name) virtual HRESULT name
#endif
struct IDirect3DDevice9;
struct IDirect3DBaseTexture9;
struct StubIDirect3DStateBlock9 {
 virtual ~StubIDirect3DStateBlock9()=default;
 virtual HRESULT GetDevice(IDirect3DDevice9**)=0;
 virtual HRESULT Capture()=0;
 virtual HRESULT Apply()=0;
};
'''
    return base+text+'\nStateBlock* recording_=nullptr;bool ffp_dirty_=true;\n'
