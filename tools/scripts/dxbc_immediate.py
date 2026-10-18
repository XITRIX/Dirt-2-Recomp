#!/usr/bin/env python3
"""Generates shims/d3d11/immediate_shaders.inc: SM 5.0 DXBC for the 8 shaders
of the PSSG engine's immediate-mode HLSL file, which the DX11 path compiles
with D3DCompile at run time (there is no HLSL compiler here; see the D3D9
path's sm3asm.py for the same file in SM 3.0).

The file (entry points vs/psUntextured, vs/psPrelitUntextured, vs/psTextured,
vs/psPrelitTextured) declares $Globals { float4x4 modelviewproj; float4
diffuse; }, Texture2D texture0 and SamplerState samplerLinear. Each blob
carries RDEF (the game reflects the result for the constant offsets), ISGN,
OSGN and SHEX. The DXBC checksum is left zero: DXMT does not check it.

Usage: dxbc_immediate.py <out.inc>
"""
import struct
import sys

# ---- tokenized program format (d3d11TokenizedProgramFormat.hpp) ----
OP = dict(add=0, dp4=17, mad=50, mov=54, mul=56, ret=62, sample=69, dcl_resource=88, dcl_constantbuffer=89,
          dcl_sampler=90, dcl_input=95, dcl_input_ps=98, dcl_input_ps_siv=100, dcl_output=101, dcl_output_siv=103,
          dcl_temps=104, dcl_global_flags=106)
T_TEMP, T_INPUT, T_OUTPUT, T_SAMPLER, T_RESOURCE, T_CB = 0, 1, 2, 6, 7, 8
SW = {"x": 0, "y": 1, "z": 2, "w": 3}


def swz(s):
    s = (s + s[-1] * 4)[:4]
    return SW[s[0]] | SW[s[1]] << 2 | SW[s[2]] << 4 | SW[s[3]] << 6


def mask(s):
    return sum(1 << SW[ch] for ch in s)


def operand(typ, idx, sel=None, comps=2):
    """sel: ('m', 'xyzw') dest mask, ('s', 'xyzw') source swizzle, None for 0 components."""
    tok = typ << 12 | len(idx) << 20
    if comps == 0:
        pass
    else:
        tok |= 2  # 4 components
        if sel[0] == "m":
            tok |= 0 << 2 | mask(sel[1]) << 4
        else:
            tok |= 1 << 2 | swz(sel[1]) << 4
    return [tok] + list(idx)


def instr(op, *operands, ctrl=0, extra=()):
    body = [t for o in operands for t in o] + list(extra)
    return [OP[op] | ctrl << 11 | (1 + len(body)) << 24] + body


def r(i, sel):
    return operand(T_TEMP, [i], sel)


def v(i, sel):
    return operand(T_INPUT, [i], sel)


def o(i, sel):
    return operand(T_OUTPUT, [i], sel)


def cb(reg, sel="xyzw"):
    return operand(T_CB, [0, reg], ("s", sel))


def mvp(dst):
    """dst = mul(modelviewproj, v0), column_major: sum of cb0[k] * v0.k."""
    return (instr("mul", r(0, ("m", "xyzw")), v(0, ("s", "yyyy")), cb(1)) +
            instr("mad", r(0, ("m", "xyzw")), cb(0), v(0, ("s", "xxxx")), r(0, ("s", "xyzw"))) +
            instr("mad", r(0, ("m", "xyzw")), cb(2), v(0, ("s", "zzzz")), r(0, ("s", "xyzw"))) +
            instr("mad", dst, cb(3), v(0, ("s", "wwww")), r(0, ("s", "xyzw"))))


DCL_CB = instr("dcl_constantbuffer", operand(T_CB, [0, 5], ("s", "xyzw")))
DCL_TEX = instr("dcl_resource", operand(T_RESOURCE, [0], comps=0), ctrl=3, extra=[0x5555])  # texture2d, float x4
DCL_SAMP = instr("dcl_sampler", operand(T_SAMPLER, [0], comps=0))
FLAGS = [OP["dcl_global_flags"] | 1 << 11 | 1 << 24]  # refactoringAllowed


def dcl_in(i, m):
    return instr("dcl_input", v(i, ("m", m)))


def dcl_in_ps(i, m):
    return instr("dcl_input_ps", v(i, ("m", m)), ctrl=2)  # linear


def dcl_out_pos():
    return instr("dcl_output_siv", o(0, ("m", "xyzw")), extra=[1])  # position


def dcl_out(i, m):
    return instr("dcl_output", o(i, ("m", m)))


def temps(n):
    return [OP["dcl_temps"] | 2 << 24, n]


RET = instr("ret")
SAMPLE_V = lambda reg: instr("sample", r(0, ("m", "xyzw")), v(reg, ("s", "xyxx")),
                             operand(T_RESOURCE, [0], ("s", "xyzw")), operand(T_SAMPLER, [0], comps=0))

# Signature elements: (semantic, index, system value, register, mask, rw mask)
POS_IN = ("POSITION", 0, 0, 0, "xyzw", "xyzw")
SV_POS_OUT = ("SV_POSITION", 0, 1, 0, "xyzw", "")
SV_POS_PS = ("SV_POSITION", 0, 1, 0, "xyzw", "")
TARGET = ("SV_TARGET", 0, 0, 0, "xyzw", "")

# entry -> (vertex?, inputs, outputs, uses globals, uses texture, body)
SHADERS = {
    "vsUntextured": (True, [POS_IN], [SV_POS_OUT], True, False,
                     FLAGS + DCL_CB + dcl_in(0, "xyzw") + dcl_out_pos() + temps(1) + mvp(o(0, ("m", "xyzw"))) + RET),
    "psUntextured": (False, [SV_POS_PS], [TARGET], True, False,
                     FLAGS + DCL_CB + dcl_out(0, "xyzw") + instr("mov", o(0, ("m", "xyzw")), cb(4)) + RET),
    "vsPrelitUntextured": (True, [POS_IN, ("COLOR", 0, 0, 1, "xyzw", "xyzw")],
                           [SV_POS_OUT, ("COLOR", 0, 0, 1, "xyzw", "")], True, False,
                           FLAGS + DCL_CB + dcl_in(0, "xyzw") + dcl_in(1, "xyzw") + dcl_out_pos() + dcl_out(1, "xyzw") +
                           temps(1) + mvp(o(0, ("m", "xyzw"))) + instr("mov", o(1, ("m", "xyzw")), v(1, ("s", "xyzw"))) + RET),
    "psPrelitUntextured": (False, [SV_POS_PS, ("COLOR", 0, 0, 1, "xyzw", "xyzw")], [TARGET], False, False,
                           FLAGS + dcl_in_ps(1, "xyzw") + dcl_out(0, "xyzw") +
                           instr("mov", o(0, ("m", "xyzw")), v(1, ("s", "xyzw"))) + RET),
    "vsTextured": (True, [POS_IN, ("TEXCOORD", 0, 0, 1, "xy", "xy")],
                   [SV_POS_OUT, ("TEXCOORD", 0, 0, 1, "xy", "")], True, False,
                   FLAGS + DCL_CB + dcl_in(0, "xyzw") + dcl_in(1, "xy") + dcl_out_pos() + dcl_out(1, "xy") + temps(1) +
                   mvp(o(0, ("m", "xyzw"))) + instr("mov", o(1, ("m", "xy")), v(1, ("s", "xyxx"))) + RET),
    "psTextured": (False, [SV_POS_PS, ("TEXCOORD", 0, 0, 1, "xy", "xy")], [TARGET], False, True,
                   FLAGS + DCL_SAMP + DCL_TEX + dcl_in_ps(1, "xy") + dcl_out(0, "xyzw") + temps(1) + SAMPLE_V(1) +
                   instr("mov", o(0, ("m", "xyzw")), r(0, ("s", "xyzw"))) + RET),
    # POSITION output is SV_Position under D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY (the game sets it).
    "vsPrelitTextured": (True, [POS_IN, ("TEXCOORD", 0, 0, 1, "xy", "xy"), ("COLOR", 0, 0, 2, "xyzw", "xyzw")],
                         [SV_POS_OUT, ("COLOR", 0, 0, 1, "xyzw", ""), ("TEXCOORD", 0, 0, 2, "xy", "")], True, False,
                         FLAGS + DCL_CB + dcl_in(0, "xyzw") + dcl_in(1, "xy") + dcl_in(2, "xyzw") + dcl_out_pos() +
                         dcl_out(1, "xyzw") + dcl_out(2, "xy") + temps(1) + mvp(o(0, ("m", "xyzw"))) +
                         instr("mov", o(1, ("m", "xyzw")), v(2, ("s", "xyzw"))) +
                         instr("mov", o(2, ("m", "xy")), v(1, ("s", "xyxx"))) + RET),
    "psPrelitTextured": (False, [SV_POS_PS, ("COLOR", 0, 0, 1, "xyzw", "xyzw"), ("TEXCOORD", 0, 0, 2, "xy", "xy")],
                         [TARGET], False, True,
                         FLAGS + DCL_SAMP + DCL_TEX + dcl_in_ps(1, "xyzw") + dcl_in_ps(2, "xy") + dcl_out(0, "xyzw") +
                         temps(1) + SAMPLE_V(2) +
                         instr("mul", o(0, ("m", "xyzw")), v(1, ("s", "xyzw")), r(0, ("s", "xyzw"))) + RET),
}


class Strings:
    def __init__(self, base):
        self.base, self.data, self.at = base, b"", {}

    def add(self, s):
        if s not in self.at:
            self.at[s] = self.base + len(self.data)
            self.data += s.encode() + b"\0"
        return self.at[s]


def signature(elems):
    n = len(elems)
    strs = Strings(8 + 24 * n)
    out = struct.pack("<II", n, 8)
    for name, idx, sv, reg, m, rw in elems:
        out += struct.pack("<IIIIIBBH", strs.add(name), idx, sv, 3, reg, mask(m), mask(rw) if rw else 0, 0)
    data = out + strs.data
    return data + b"\0" * (-len(data) % 4)


def rdef(vertex, globals_, texture):
    # Layout: header (28) + RD11 (32), bindings, cbuffer, variables, types, strings.
    binds = []
    if texture:
        binds += [("samplerLinear", 3, 0, 0, 0), ("texture0", 2, 5, 4, 0xFFFFFFFF)]
    if globals_:
        binds += [("$Globals", 0, 0, 0, 0)]
    head = 60
    bind_off = head
    cb_off = bind_off + 32 * len(binds)
    var_off = cb_off + (24 if globals_ else 0)
    type_off = var_off + (80 if globals_ else 0)
    str_base = type_off + (72 if globals_ else 0)
    strs = Strings(str_base)
    creator = strs.add("DiRT 2 recompilation (tools/scripts/dxbc_immediate.py)")
    out = struct.pack("<IIIIBBHII", 1 if globals_ else 0, cb_off if globals_ else 0, len(binds), bind_off, 0, 5,
                      0xFFFE if vertex else 0xFFFF, 0x100, creator)
    out += b"RD11" + struct.pack("<7I", 60, 24, 32, 40, 36, 12, 0)
    for name, typ, ret, dim, samples in binds:
        out += struct.pack("<8I", strs.add(name), typ, ret, dim, samples, 0, 1, 0 if typ != 3 else 0)
    if globals_:
        out += struct.pack("<6I", strs.add("$Globals"), 2, var_off, 80, 0, 0)
        out += struct.pack("<10I", strs.add("modelviewproj"), 0, 64, 2, type_off, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0)
        out += struct.pack("<10I", strs.add("diffuse"), 64, 16, 2, type_off + 36, 0, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0)
        # D3D_SVC_MATRIX_COLUMNS=3 / D3D_SVC_VECTOR=1, D3D_SVT_FLOAT=3
        out += struct.pack("<HHHHHHI4II", 3, 3, 4, 4, 0, 0, 0, 0, 0, 0, 0, strs.add("float4x4"))
        out += struct.pack("<HHHHHHI4II", 1, 3, 1, 4, 0, 0, 0, 0, 0, 0, 0, strs.add("float4"))
    assert len(out) == str_base, (len(out), str_base)
    data = out + strs.data
    return data + b"\0" * (-len(data) % 4)


def shex(vertex, body):
    toks = [(1 if vertex else 0) << 16 | 5 << 4 | 0, 2 + len(body)] + body
    return struct.pack("<%dI" % len(toks), *toks)


def container(chunks):
    n = len(chunks)
    offs, data = [], b""
    pos = 32 + 4 * n
    for tag, payload in chunks:
        offs.append(pos + len(data))
        data += tag + struct.pack("<I", len(payload)) + payload
    total = 32 + 4 * n + len(data)
    return b"DXBC" + b"\0" * 16 + struct.pack("<III", 1, total, n) + struct.pack("<%dI" % n, *offs) + data


def main():
    out = ["// Generated by tools/scripts/dxbc_immediate.py. Do not edit.",
           "// SM 5.0 DXBC for the PSSG immediate-mode shaders (D3DCompile on the DX11 path).", ""]
    entries = []
    for name, (vertex, ins, outs, glob, tex, body) in SHADERS.items():
        blob = container([(b"RDEF", rdef(vertex, glob, tex)), (b"ISGN", signature(ins)), (b"OSGN", signature(outs)),
                          (b"SHEX", shex(vertex, body))])
        hexes = ", ".join("0x%02x" % b for b in blob)
        out.append(f"static const uint8_t kImm_{name}[] = {{{hexes}}};")
        entries.append(f'    {{"{name}", "{"vs" if vertex else "ps"}_5_0", kImm_{name}, sizeof kImm_{name}}},')
    out.append("struct ImmediateShader { const char* entry; const char* target; const uint8_t* code; uint32_t size; };")
    out.append("static const ImmediateShader kImmediateShaders[] = {")
    out += entries
    out.append("};")
    open(sys.argv[1], "w").write("\n".join(out) + "\n")
    print(f"{len(entries)} shaders")


if __name__ == "__main__":
    main()
