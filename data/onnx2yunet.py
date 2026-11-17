"""YuNet ONNX -> C header for src/yunet.c (run by build.sh). Reads the protobuf by hand: no onnx package needed.
   python3 onnx2yunet.py face_detection_yunet_2023mar.onnx out.h

Emits the weights as one float array and the graph as a list of ops on numbered tensors (0 = the BGR input):
conv (ReLU folded in when the next node is a Relu), maxpool 2x2, nearest upsample x2, add. The heads stop before
their Transpose / Reshape / Sigmoid (yunet.c reads the conv outputs directly and applies the sigmoid itself)."""
import struct
import sys


def fields(buf):
    """(field number, wire type, value) for each field of a protobuf message; value is int or bytes."""
    i, n = 0, len(buf)
    while i < n:
        key, i = varint(buf, i)
        f, wt = key >> 3, key & 7
        if wt == 0:
            v, i = varint(buf, i)
        elif wt == 1:
            v, i = buf[i:i + 8], i + 8
        elif wt == 2:
            ln, i = varint(buf, i)
            v, i = buf[i:i + ln], i + ln
        elif wt == 5:
            v, i = buf[i:i + 4], i + 4
        else:
            raise ValueError(f"wire type {wt}")
        yield f, wt, v


def varint(buf, i):
    r, s = 0, 0
    while True:
        b = buf[i]
        i += 1
        r |= (b & 0x7F) << s
        s += 7
        if b < 0x80:
            return r, i


def packed_ints(v):
    out, i = [], 0
    while i < len(v):
        x, i = varint(v, i)
        out.append(x - (1 << 64) if x >= 1 << 63 else x)
    return out


def tensor(buf):
    t = {"dims": [], "f": [], "i": [], "raw": b"", "type": 1, "name": ""}
    for f, wt, v in fields(buf):
        if f == 1:
            t["dims"] += packed_ints(v) if wt == 2 else [v]
        elif f == 2:
            t["type"] = v
        elif f == 4:
            t["f"] += list(struct.unpack(f"<{len(v) // 4}f", v)) if wt == 2 else [struct.unpack("<f", v)[0]]
        elif f == 7:
            t["i"] += packed_ints(v) if wt == 2 else [v]
        elif f == 8:
            t["name"] = v.decode()
        elif f == 9:
            t["raw"] = v
    if t["raw"]:
        t["f" if t["type"] == 1 else "i"] = list(struct.unpack(f"<{len(t['raw']) // 4}f", t["raw"])) if t["type"] == 1 else \
            list(struct.unpack(f"<{len(t['raw']) // 8}q", t["raw"]))
    return t


def attr(buf):
    a = {"name": "", "i": None, "ints": [], "s": b"", "f": None, "floats": []}
    for f, wt, v in fields(buf):
        if f == 1:
            a["name"] = v.decode()
        elif f == 2:
            a["f"] = struct.unpack("<f", v)[0]
        elif f == 3:
            a["i"] = v
        elif f == 4:
            a["s"] = v
        elif f == 8:
            a["ints"] += packed_ints(v) if wt == 2 else [v]
    return a


def node(buf):
    n = {"in": [], "out": [], "op": "", "attr": {}}
    for f, wt, v in fields(buf):
        if f == 1:
            n["in"].append(v.decode())
        elif f == 2:
            n["out"].append(v.decode())
        elif f == 4:
            n["op"] = v.decode()
        elif f == 5:
            a = attr(v)
            n["attr"][a["name"]] = a
    return n


def main(src, dst):
    model = open(src, "rb").read()
    graph = next(v for f, wt, v in fields(model) if f == 7)
    nodes, inits = [], {}
    for f, wt, v in fields(graph):
        if f == 1:
            nodes.append(node(v))
        elif f == 5:
            t = tensor(v)
            inits[t["name"]] = t

    tid = {"input": 0}
    def T(name):
        return tid.setdefault(name, len(tid))

    W, ops, heads = [], [], {}
    consumers = {}
    for k, n in enumerate(nodes):
        for i in n["in"]:
            consumers.setdefault(i, []).append(k)
    relu_folded = set()
    for k, n in enumerate(nodes):
        op = n["op"]
        if op == "Conv":
            w, b = inits[n["in"][1]], inits[n["in"][2]]
            a = n["attr"]
            cout, cin_g, kh, kw = w["dims"]
            g = a["group"]["i"] if "group" in a else 1
            st = a["strides"]["ints"][0]
            pad = a["pads"]["ints"][0] if "pads" in a else 0
            assert kh == kw and a["strides"]["ints"][0] == a["strides"]["ints"][1]
            woff = len(W)
            W += w["f"]
            boff = len(W)
            W += b["f"]
            out = n["out"][0]
            nxt = consumers.get(out, [])
            relu = len(nxt) == 1 and nodes[nxt[0]]["op"] == "Relu"
            if relu:
                relu_folded.add(nxt[0])
                tid[nodes[nxt[0]]["out"][0]] = T(out)       # the Relu's output is the conv's (in place)
            ops.append(("YN_CONV", T(n["in"][0]), -1, T(out), cin_g * g, cout, kh, st, pad, g, woff, boff, int(relu)))
        elif op == "Relu":
            assert k in relu_folded, "a Relu not straight after a Conv"
        elif op == "MaxPool":
            assert n["attr"]["kernel_shape"]["ints"] == [2, 2] and n["attr"]["strides"]["ints"] == [2, 2]
            ops.append(("YN_POOL", T(n["in"][0]), -1, T(n["out"][0]), 0, 0, 2, 2, 0, 1, 0, 0, 0))
        elif op == "Resize":
            assert n["attr"]["mode"]["s"] == b"nearest" and inits[n["in"][2]]["f"] == [1.0, 1.0, 2.0, 2.0]
            ops.append(("YN_UP2", T(n["in"][0]), -1, T(n["out"][0]), 0, 0, 0, 0, 0, 1, 0, 0, 0))
        elif op == "Add":
            ops.append(("YN_ADD", T(n["in"][0]), T(n["in"][1]), T(n["out"][0]), 0, 0, 0, 0, 0, 1, 0, 0, 0))
        elif op == "Transpose":
            heads[n["out"][0]] = n["in"][0]                   # remember which conv feeds this head
        elif op in ("Reshape", "Sigmoid"):
            heads[n["out"][0]] = heads[n["in"][0]]
        else:
            raise ValueError("unsupported op " + op)

    outs = {}
    for f, wt, v in fields(graph):
        if f == 12:
            name = next(x for g2, w2, x in fields(v) if g2 == 1).decode()
            outs[name] = tid[heads[name]]
    kinds = ["cls", "obj", "bbox", "kps"]
    table = [[outs[f"{k}_{s}"] for s in (8, 16, 32)] for k in kinds]

    o = [f"// made from {src.split('/')[-1]} by data/onnx2yunet.py (YuNet, MIT licence: see data/README.md)",
         "enum { YN_CONV, YN_POOL, YN_UP2, YN_ADD };",
         f"#define YN_NT {len(tid)}", f"#define YN_NOPS {len(ops)}",
         "struct yn_op { int op, a, b, out, cin, cout, k, stride, pad, group, w, bias, relu; };",
         "static const struct yn_op YN_OPS[YN_NOPS] = {",
         ",\n".join("{" + ",".join(str(x) for x in op) + "}" for op in ops), "};",
         "// conv outputs feeding cls / obj / bbox / kps at strides 8, 16, 32",
         "static const int YN_HEAD[4][3] = {" + ",".join("{" + ",".join(map(str, r)) + "}" for r in table) + "};",
         f"static const float YN_W[{len(W)}] = {{"]
    for i in range(0, len(W), 8):
        o.append(",".join("%.9gf" % x for x in W[i:i + 8]) + ",")
    o += ["};", ""]
    open(dst, "w").write("\n".join(o))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
