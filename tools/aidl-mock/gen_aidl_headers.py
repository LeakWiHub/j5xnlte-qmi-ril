#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generates C++ headers in the style of the AIDL NDK backend from FROZEN .aidl snapshots
(hardware/interfaces/radio/aidl/aidl_api/<pkg>/<ver>/...). They are used ONLY to type-check the HAL
sources on a PC (names of fields/enumerators, method signatures). They are not ABI-compatible with the
real generated code and are never shipped to the device; the real build uses the aidl compiler.

usage: gen_aidl_headers.py <aidl_api_dir> <version> <out_include_dir>
"""
import os, re, sys

API = VER = OUT = None

def configure(api, ver, out):
    global API, VER, OUT
    API, VER, OUT = api, ver, out
PKGS = ["", ".config", ".data", ".messaging", ".modem", ".network", ".sim", ".voice"]
PRIM = {"int": "int32_t", "long": "int64_t", "boolean": "bool", "byte": "int8_t", "char": "char16_t",
        "float": "float", "double": "double"}

types = {}      # fq name -> dict(kind, backing, node)
files = []      # (pkg, text)

def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)

def split_top(s, sep=","):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "(<[{": depth += 1
        if ch in ")>]}": depth -= 1
        if ch == sep and depth == 0:
            out.append(cur); cur = ""
        else:
            cur += ch
    if cur.strip(): out.append(cur)
    return out

def parse_body(text, i, kind):
    """parse members from text starting after '{' until matching '}'. returns (members, next_index)."""
    members, buf = [], ""
    pending_ann = ""
    while i < len(text):
        ch = text[i]
        if ch == "}":
            return members, i + 1
        if ch == "{" and buf.rstrip().endswith("="):   # field initializer such as "= {}"
            depth, j = 0, i
            while True:
                depth += text[j] == "{"
                depth -= text[j] == "}"
                j += 1
                if depth == 0: break
            buf += text[i:j]
            i = j
            continue
        if ch == "{":
            head = buf.strip(); buf = ""
            m = re.match(r"^(?P<ann>(?:@[\w.]+(?:\([^)]*\))?\s*)*)(?P<k>parcelable|enum|union|interface)\s+(?P<n>\w+)$", head, re.S)
            if not m: raise SystemExit("cannot parse decl: %r" % head)
            sub, i = parse_body(text, i + 1, m.group("k"))
            members.append(("type", m.group("k"), m.group("n"), m.group("ann"), sub))
            continue
        if ch == "," and kind != "enum":
            buf += ch; i += 1
            continue
        if ch == ";" or (ch == "," and kind == "enum"):
            stmt = buf.strip(); buf = ""
            i += 1
            if kind == "enum" and ch == ",":
                if stmt: members.append(("enumv", stmt))
                continue
            if kind == "enum" and ch == ";": continue
            if not stmt: continue
            members.append(("stmt", stmt)); continue
        buf += ch; i += 1
    return members, i

def load():
    for p in PKGS:
        d = os.path.join(API, "android.hardware.radio" + p, VER)
        for root, _, fs in os.walk(d):
            for f in fs:
                if not f.endswith(".aidl"): continue
                t = strip_comments(open(os.path.join(root, f)).read())
                pkg = re.search(r"^\s*package\s+([\w.]+);", t, re.M).group(1)
                t = t[t.index(";", t.index("package")) + 1:]
                m = re.match(r"\s*((?:@[\w.]+(?:\([^)]*\))?\s*)*)(parcelable|enum|union|interface)\s+(\w+)\s*\{", t)
                if not m: raise SystemExit("bad file " + f)
                body, _ = parse_body(t, m.end(), m.group(2))
                files.append((pkg, m.group(2), m.group(3), m.group(1), body))

def register(pkg, kind, name, ann, body, prefix=""):
    fq = pkg + "." + prefix + name
    backing = "int32_t"
    mb = re.search(r'@Backing\(type="(\w+)"\)', ann)
    if mb: backing = PRIM[mb.group(1)]
    types[fq] = {"kind": kind, "backing": backing}
    for m in body:
        if m[0] == "type":
            register(pkg, m[1], m[2], m[3], m[4], prefix + name + ".")

def cpp_name(fq): return "::aidl::" + fq.replace(".", "::")

def is_enum(t): return types.get(t, {}).get("kind") == "enum"
def is_iface(t): return types.get(t, {}).get("kind") == "interface"

def base_type(t):
    t = t.strip()
    m = re.match(r"^List<(.+)>$", t)
    if m: return "vec", m.group(1)
    if t.endswith("[]"): return "vec", t[:-2]
    return "one", t

def elem(t, pkg, scope):
    if t in PRIM: return "uint8_t" if False else PRIM[t]
    if t == "String": return "std::string"
    fq = resolve(t, pkg, scope)
    if is_iface(fq): return "std::shared_ptr<%s>" % cpp_name(fq)
    return cpp_name(fq)

def resolve(t, pkg, scope):
    if t in types: return t
    for k in range(len(scope), -1, -1):
        c = pkg + "." + ".".join(scope[:k] + [t]) if k or True else t
        if c in types: return c
    c = pkg + "." + t
    if c in types: return c
    raise SystemExit("unresolved type %s in %s" % (t, pkg))

def field_type(t, nullable, pkg, scope):
    kind, inner = base_type(t)
    if kind == "vec":
        e = "uint8_t" if inner == "byte" else elem(inner, pkg, scope)
        r = "std::vector<%s>" % e
    else:
        r = elem(inner, pkg, scope)
    return "std::optional<%s>" % r if nullable else r

def by_value(t, nullable, pkg, scope):
    kind, inner = base_type(t)
    if kind == "one" and not nullable:
        if inner in PRIM: return PRIM[inner]
        if inner != "String" and is_enum(resolve(inner, pkg, scope)): return cpp_name(resolve(inner, pkg, scope))
    return "const %s&" % field_type(t, nullable, pkg, scope)

def conv_default(expr, ftype):
    e = expr.strip()
    e = re.sub(r"(0x[0-9A-Fa-f]+)u8", r"\1", e)
    if re.match(r"^-?(0x[0-9A-Fa-f]+|\d+)(\.\d+)?f?$", e) or e in ("true", "false") or re.match(r'^".*"$', e):
        if ftype.startswith("::aidl"): return "static_cast<%s>(%s)" % (ftype, e)
        return e
    if re.match(r"^[A-Za-z_][\w.]*$", e) and "." in e:
        parts = e.split(".")
        return "::aidl::" + "::".join(parts[:-1]) + "::" + parts[-1]
    return None

def parse_param(p):
    p = p.strip()
    nullable = "@nullable" in p
    p = re.sub(r"@\w+(\([^)]*\))?", "", p)
    p = re.sub(r"\b(in|out|inout)\b", "", p).strip()
    t, n = p.rsplit(None, 1)
    return t.strip(), n.strip(), nullable

def emit_type(pkg, kind, name, ann, body, scope, out_includes, ind=""):
    L = []
    me = scope + [name]
    sub = [m for m in body if m[0] == "type"]
    if kind == "enum":
        backing = types[pkg + "." + ".".join(me)]["backing"]
        L.append("%senum class %s : %s {" % (ind, name, backing))
        for m in body:
            if m[0] == "enumv":
                v = re.sub(r"(0x[0-9A-Fa-f]+)u8", r"\1", m[1])
                v = re.sub(r"\b((?:[a-z]\w*\.)+[A-Z]\w*(?:\.[A-Z_0-9a-z]+)+)\b",
                           lambda mm: "static_cast<int32_t>(::aidl::%s::%s)" % ("::".join(mm.group(1).split(".")[:-1]), mm.group(1).split(".")[-1]), v)
                L.append("%s  %s," % (ind, v.replace("\n", " ")))
        L.append("%s};" % ind)
        return L
    if kind == "interface":
        raise SystemExit("nested interface unsupported")
    L.append("%sclass %s {" % (ind, name))
    L.append("%s public:" % ind)
    for m in sub:
        L += emit_type(pkg, m[1], m[2], m[3], m[4], me, out_includes, ind + "  ")
    if kind == "parcelable":
        for m in body:
            if m[0] != "stmt": continue
            s = " ".join(m[1].split())
            mc = re.match(r"^const\s+(\w+)\s+(\w+)\s*=\s*(.+)$", s)
            if mc:
                t, n, v = mc.groups()
                v = re.sub(r"(0x[0-9A-Fa-f]+)u8", r"\1", v)
                L.append("%s  static constexpr %s %s = static_cast<%s>(%s);" % (ind, PRIM[t], n, PRIM[t], conv_default(v, PRIM[t]) or v))
                continue
            nullable = "@nullable" in s
            s2 = re.sub(r"@\w+(\([^)]*\))?\s*", "", s)
            md = re.match(r"^(.+?)\s+(\w+)(?:\s*=\s*(.+))?$", s2)
            t, n, dflt = md.groups()
            ft = field_type(t.strip(), nullable, pkg, me)
            init = "{}"
            if dflt:
                c = conv_default(dflt, ft)
                if c: init = " = " + c
                else: init = "{}"
            L.append("%s  %s %s%s;" % (ind, ft, n, init if init.startswith(" =") else "{}"))
    else:  # union
        fields = []
        for m in body:
            if m[0] != "stmt": continue
            s2 = re.sub(r"@\w+(\([^)]*\))?\s*", "", " ".join(m[1].split()))
            t, n = s2.rsplit(None, 1)
            fields.append((n, field_type(t.strip(), "@nullable" in m[1], pkg, me)))
        L.append("%s  enum Tag : int32_t { %s };" % (ind, ", ".join("%s = %d" % (n, i) for i, (n, _) in enumerate(fields))))
        L.append("%s  std::variant<%s> _v;" % (ind, ", ".join(t for _, t in fields)))
        L.append("%s  template <Tag tag, typename T> static %s make(T&& v) { %s r; r._v.template emplace<(size_t)tag>(std::forward<T>(v)); return r; }" % (ind, name, name))
        L.append("%s  Tag getTag() const { return (Tag)_v.index(); }" % ind)
        L.append("%s  template <Tag tag> const auto& get() const { return std::get<(size_t)tag>(_v); }" % ind)
    L.append("%s};" % ind)
    return L

def refs_of(pkg, body, scope):
    found = set()
    def walk(b, sc):
        for m in b:
            if m[0] == "type":
                walk(m[4], sc + [m[2]])
            elif m[0] == "enumv":
                for tok in re.findall(r"[A-Za-z_][\w.]*", m[1]):
                    parts = tok.split(".")
                    for k in range(len(parts) - 1, 0, -1):
                        c = ".".join(parts[:k])
                        if c in types: found.add(c); break
            elif m[0] == "stmt":
                s = re.sub(r"@\w+(\([^)]*\))?\s*", "", " ".join(m[1].split()))
                init = s.split("=", 1)[1] if "=" in s else ""
                for tok in re.findall(r"[A-Za-z_][\w.]*", init):   # types named only in a default value
                    parts = tok.split(".")
                    for k in range(len(parts) - 1, 0, -1):
                        c = ".".join(parts[:k])
                        if c in types: found.add(c); break
                s = re.sub(r"=.*$", "", s)
                mc = re.match(r"^(const\s+)?(.+?)\s+\w+(\(.*\))?$", s)
                for tok in re.findall(r"[A-Za-z_][\w.]*", s):
                    tok2 = tok.rstrip(".")
                    for c in (tok2, pkg + "." + tok2):
                        if c in types:
                            found.add(c.split(".")[0] and c)
    walk(body, scope)
    return found

def top_of(fq):
    # reduce nested name to its top-level type fq (pkg + first segment of remaining) : types registered with dots
    parts = fq.split(".")
    for k in range(len(parts), 0, -1):
        c = ".".join(parts[:k])
        if c in top_level: return c
    return fq

def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "w").write(text)

def main():
    global top_level
    load()
    top_level = set()
    for pkg, kind, name, ann, body in files:
        top_level.add(pkg + "." + name)
        register(pkg, kind, name, ann, body)
    pre = "// GENERATED mock header - type checking only, not ABI compatible. SPDX-License-Identifier: Apache-2.0\n#pragma once\n#include <cstdint>\n#include <memory>\n#include <optional>\n#include <string>\n#include <variant>\n#include <vector>\n#include <android/binder_auto_utils.h>\n"
    for pkg, kind, name, ann, body in files:
        inc = set()
        for r in refs_of(pkg, body, [name]):
            t = top_of(r)
            if t != pkg + "." + name: inc.add(t)
        def hdr(fq): return "aidl/" + fq.replace(".", "/") + ".h"
        if kind == "interface":
            L = [pre]
            deps = set()
            for m in body:
                if m[0] != "stmt": continue
                mm = re.match(r"^oneway void (\w+)\((.*)\)$", " ".join(m[1].split()))
                if not mm: raise SystemExit("bad method " + m[1])
                for p in split_top(mm.group(2)):
                    t, n, nl = parse_param(p)
                    kindb, inner = base_type(t)
                    if inner not in PRIM and inner != "String": deps.add(top_of(resolve(inner, pkg, [name])))
            for d in sorted(deps): L.append('#include "%s"' % hdr(d))
            ns = pkg.split(".")
            L.append("namespace " + " { namespace ".join(["aidl"] + ns) + " {")
            L.append("class %s : public ::ndk::ICInterface {\n public:\n  static constexpr const char* descriptor = \"%s.%s\";" % (name, pkg, name))
            for m in body:
                if m[0] != "stmt": continue
                mm = re.match(r"^oneway void (\w+)\((.*)\)$", " ".join(m[1].split()))
                ps = []
                for p in split_top(mm.group(2)):
                    t, n, nl = parse_param(p)
                    kk, inner = base_type(t)
                    ty = by_value(t, nl, pkg, [name])
                    if kk == "one" and inner not in PRIM and inner != "String" and is_iface(resolve(inner, pkg, [name])):
                        ty = "const std::shared_ptr<%s>&" % cpp_name(resolve(inner, pkg, [name]))
                    ps.append("%s in_%s" % (ty, n))
                L.append("  virtual ::ndk::ScopedAStatus %s(%s) = 0;" % (mm.group(1), ", ".join(ps)))
            L.append("};")
            L.append("}" + " }" * (len(ns)))
            write(os.path.join(OUT, hdr(pkg + "." + name)), "\n".join(L) + "\n")
            bn = "Bn" + name[1:]
            L = [pre, '#include "%s"' % hdr(pkg + "." + name), "namespace " + " { namespace ".join(["aidl"] + ns) + " {",
                 "class %s : public ::ndk::BnCInterface<%s> {};" % (bn, name), "}" + " }" * len(ns)]
            write(os.path.join(OUT, hdr(pkg + "." + bn)), "\n".join(L) + "\n")
        else:
            L = [pre]
            for d in sorted(inc): L.append('#include "%s"' % hdr(d))
            ns = pkg.split(".")
            L.append("namespace " + " { namespace ".join(["aidl"] + ns) + " {")
            L += emit_type(pkg, kind, name, ann, body, [], inc)
            L.append("}" + " }" * len(ns))
            write(os.path.join(OUT, hdr(pkg + "." + name)), "\n".join(L) + "\n")
    print("generated %d top-level types from V%s into %s" % (len(files), VER, OUT))

if __name__ == "__main__":
    configure(*sys.argv[1:4])
    main()
