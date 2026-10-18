#!/usr/bin/env python3
"""DiRT 2 system/states.bin (compiled frontend XML: screens, menus, rows).

Layout, all little-endian, every chunk = u32 tag (0x7252221x) + u32 size:
  0x1a  file
    0x17  strings: 0x1d = NUL-terminated strings, 0x1e = u32 offset per string index
    0x1b  nodes: 24-byte records (name, text, attr_count, first_attr, child_count, first_child);
          text is a string index, 0 for none; first_attr is 0 without attributes
    0x1c  attributes: (name, value) string-index pairs
Node 0 is the root. Nodes are visited depth first; a visited node's children
take the next block of slots, so siblings are contiguous; attributes are in
visiting order.

Usage:
  states_bin.py dump <states.bin> [node name or id=VALUE]   print as XML
  states_bin.py roundtrip <states.bin>                       decode + encode, compare
As a module: load() -> Tree, Tree.encode() -> bytes.
"""
import struct
import sys

TAG = 0x72522200


class Node:
    __slots__ = ("name", "attrs", "children", "parent", "text")

    def __init__(self, name, attrs=None, text=None):
        self.name = name
        self.text = text  # character data, None when empty
        self.attrs = attrs or []  # [(name, value)]
        self.children = []
        self.parent = None

    def get(self, key):
        for k, v in self.attrs:
            if k == key:
                return v
        return None

    def add(self, child, index=None):
        child.parent = self
        if index is None:
            self.children.append(child)
        else:
            self.children.insert(index, child)
        return child

    def find(self, pred):
        if pred(self):
            return self
        for c in self.children:
            r = c.find(pred)
            if r:
                return r
        return None


class Tree:
    def __init__(self, root, strings):
        self.root = root
        self.strings = strings  # original string order, kept so a round trip is identical

    def encode(self):
        strings = list(self.strings)
        index = {s: i for i, s in enumerate(strings)}

        def sid(s):
            if s not in index:
                index[s] = len(strings)
                strings.append(s)
            return index[s]

        # Layout: nodes are visited depth first; when a node is visited, its
        # children get the next block of slots (so siblings are contiguous) and
        # first_child is that block's start, also for a node without children.
        # Attributes are stored in visiting (document) order.
        pos, first_child, slots = {id(self.root): 0}, {}, [1]
        attrs, first_attr = [], {}

        def walk(n):
            first_attr[id(n)] = len(attrs) // 2 if n.attrs else 0
            for k, v in n.attrs:
                attrs.extend((sid(k), sid(v)))
            first_child[id(n)] = slots[0]
            for c in n.children:
                pos[id(c)] = slots[0]
                slots[0] += 1
            for c in n.children:
                walk(c)

        walk(self.root)
        order = [None] * slots[0]
        stack = [self.root]
        while stack:
            n = stack.pop()
            order[pos[id(n)]] = n
            stack.extend(n.children)
        nodes = []
        for n in order:
            nodes.append((sid(n.name), sid(n.text) if n.text is not None else 0, len(n.attrs), first_attr[id(n)], len(n.children), first_child[id(n)]))
        blob, offsets = bytearray(), []
        for s in strings:
            offsets.append(len(blob))
            blob += s.encode("latin1") + b"\0"
        while len(blob) % 4:
            blob += b"\0"

        def chunk(tag, payload):
            return struct.pack("<II", TAG | tag, len(payload)) + payload

        strs = chunk(0x1D, bytes(blob)) + chunk(0x1E, struct.pack("<%dI" % len(offsets), *offsets))
        node_bytes = b"".join(struct.pack("<6I", *n) for n in nodes)
        body = chunk(0x17, strs) + chunk(0x1B, node_bytes) + chunk(0x1C, struct.pack("<%dI" % len(attrs), *attrs))
        return chunk(0x1A, body)


def _chunks(d, o, end):
    while o + 8 <= end:
        tag, size = struct.unpack_from("<II", d, o)
        yield tag & 0xFF, o + 8, size
        o += 8 + size


def load(path):
    d = open(path, "rb").read()
    parts = {}
    for tag, o, size in _chunks(d, 8, len(d)):
        parts[tag] = (o, size)
        if tag == 0x17:
            for t2, o2, s2 in _chunks(d, o, o + size):
                parts[t2] = (o2, s2)
    so, ss = parts[0x1D]
    blob = d[so:so + ss]
    oo, os_ = parts[0x1E]
    offs = struct.unpack_from("<%dI" % (os_ // 4), d, oo)
    strings = [blob[o:blob.index(b"\0", o)].decode("latin1") for o in offs]
    no, ns = parts[0x1B]
    recs = [struct.unpack_from("<6I", d, no + 24 * i) for i in range(ns // 24)]
    ao, as_ = parts[0x1C]
    av = struct.unpack_from("<%dI" % (as_ // 4), d, ao)
    nodes = []
    for name, text, ac, fa, _cc, _fc in recs:
        attrs = [(strings[av[2 * (fa + k)]], strings[av[2 * (fa + k) + 1]]) for k in range(ac)]
        nodes.append(Node(strings[name], attrs, strings[text] if text else None))
    for i, (_n, _p, _ac, _fa, cc, fc) in enumerate(recs):
        for c in range(fc, fc + cc):
            nodes[i].add(nodes[c])
    return Tree(nodes[0], strings)


def dump(node, depth=0, out=sys.stdout):
    attrs = "".join(' %s="%s"' % kv for kv in node.attrs)
    if node.text is not None and not node.children:
        out.write("  " * depth + "<%s%s>%s</%s>\n" % (node.name, attrs, " ".join(node.text.split()), node.name))
    elif node.children:
        out.write("  " * depth + "<%s%s>\n" % (node.name, attrs))
        for c in node.children:
            dump(c, depth + 1, out)
        out.write("  " * depth + "</%s>\n" % node.name)
    else:
        out.write("  " * depth + "<%s%s/>\n" % (node.name, attrs))


if __name__ == "__main__":
    cmd, path = sys.argv[1], sys.argv[2]
    tree = load(path)
    if cmd == "dump":
        node = tree.root
        if len(sys.argv) > 3:
            q = sys.argv[3]
            if "=" in q:
                k, v = q.split("=", 1)
                node = tree.root.find(lambda n: n.get(k) == v)
            else:
                node = tree.root.find(lambda n: n.name == q)
        dump(node)
    elif cmd == "roundtrip":
        orig = open(path, "rb").read()
        new = tree.encode()
        print("identical" if new == orig else "differs: %d vs %d bytes" % (len(new), len(orig)))
