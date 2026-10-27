import re, os, sys
roots = ["tests", "docs"]
files = []
for r in roots:
    for dp, dn, fn in os.walk(r):
        for f in fn:
            if f.endswith((".bzl", ".cpp", ".md")):
                files.append(os.path.join(dp, f))
def scan(name, pat_a, pat_b, window=4, cond=None):
    hits = []
    for p in files:
        try:
            lines = open(p, encoding="utf-8", errors="replace").read().split("\n")
        except Exception:
            continue
        for i, l in enumerate(lines):
            m = pat_a.search(l)
            if not m: continue
            for j in range(i+1, min(len(lines), i+1+window)):
                mb = pat_b(m, lines[j])
                if mb:
                    hits.append(f"{p}:{i+1}: {l.strip()[:90]}  ->  {lines[j].strip()[:80]}")
                    break
    print(f"== {name}: {len(hits)}")
    for h in hits[:12]: print("   ", h)
# field overwrite / dispose under a live local borrow of the field's value
scan("field overwrite under borrow", re.compile(r"var (\w+) := (_\w+);"),
     lambda m, l: re.search(r"\b" + re.escape(m.group(2)) + r"\s*(:=|\.Dispose\(\))", l))
# null-forgiving on a definitely-null local
scan("bang on definitely-null local", re.compile(r"var (\w+)(: [\w?<>]+)? := null;"),
     lambda m, l: re.search(r"\b" + re.escape(m.group(1)) + r"!", l))
# inline-array constant index beyond its size
def oob(m, l):
    n = int(m.group(1)); v = m.group(2)
    for k in re.finditer(r"\b" + re.escape(v) + r"\[(-?\d+)\]", l):
        if int(k.group(1)) >= n or int(k.group(1)) < 0: return True
    return False
scan("inline-array constant index OOB", re.compile(r"var (?:\w+)"), lambda m,l: False)  # placeholder
hits=0
for p in files:
    try: txt=open(p,encoding="utf-8",errors="replace").read()
    except: continue
    for m in re.finditer(r"var (\w+)\s*:\s*\[(\d+)\]\w+", txt):
        v, n = m.group(1), int(m.group(2))
        for k in re.finditer(r"\b"+re.escape(v)+r"\[(-?\d+)\]", txt[m.end():m.end()+2000]):
            if int(k.group(1))>=n or int(k.group(1))<0:
                hits+=1; print("   ", p, v, n, k.group(0)); break
print("== inline-array constant OOB (typed decl):", hits)
# OwningTask awaited twice through the same binding
scan("second await of same task binding", re.compile(r"await (\w+)\b"),
     lambda m, l: re.search(r"await " + re.escape(m.group(1)) + r"\b", l), window=6)
# literal zero divisor
c=0
for p in files:
    try: txt=open(p,encoding="utf-8",errors="replace").read()
    except: continue
    for m in re.finditer(r"[\w\)\]]\s+(/|mod)\s+0(?![.\dxXbB_])", txt):
        c+=1
print("== literal-zero divisor occurrences (any dividend):", c)
