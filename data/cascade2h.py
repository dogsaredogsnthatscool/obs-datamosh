"""OpenCV LBP cascade XML -> C header for src/facedetect.c (run by build.sh).
   python3 cascade2h.py lbpcascade_frontalface_opencv.xml out.h"""
import sys
import xml.etree.ElementTree as ET

c = ET.parse(sys.argv[1]).getroot().find("cascade")
assert c.findtext("featureType").strip() == "LBP"
W, H = int(c.findtext("width")), int(c.findtext("height"))
feats = [[int(v) for v in f.findtext("rect").split()] for f in c.find("features")]
stages, weak = [], []
for s in c.find("stages"):
    ws = s.find("weakClassifiers")
    stages.append((len(ws), float(s.findtext("stageThreshold"))))
    for w in ws:
        n = [int(v) for v in w.findtext("internalNodes").split()]
        assert n[0] == 0 and n[1] == -1 and len(n) == 11, "only stumps"
        leaf = [float(v) for v in w.findtext("leafValues").split()]
        weak.append((n[2], n[3:], leaf))

o = ["// made from " + sys.argv[1].split("/")[-1] + " by data/cascade2h.py (OpenCV, see data/README)",
     f"#define FD_W {W}", f"#define FD_H {H}",
     "static const unsigned char FD_FEAT[][4] = {" + ",".join("{%d,%d,%d,%d}" % tuple(f) for f in feats) + "};",
     "static const struct { int n; float thr; } FD_STAGE[] = {" + ",".join("{%d,%.9gf}" % s for s in stages) + "};",
     "static const struct { unsigned short f; int sub[8]; float leaf[2]; } FD_WEAK[] = {",
     ",\n".join("{%d,{%s},{%.9gf,%.9gf}}" % (f, ",".join(str(v) for v in sub), l[0], l[1]) for f, sub, l in weak),
     "};", f"#define FD_NSTAGE {len(stages)}", ""]
open(sys.argv[2], "w").write("\n".join(o))
