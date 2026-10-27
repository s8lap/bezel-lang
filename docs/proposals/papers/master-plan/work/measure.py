#!/usr/bin/env python3
"""Measure the s8lap/bezel-lang issue tracker from the JSONL that fetch_issues.sh wrote.

Usage: python3 -I measure.py [--asof YYYY-MM-DD] > out/measurements.txt
Also writes out/clusters.tsv (one row per open issue: number, clusters, severity, age) so a
campaign's member list can be re-derived, and out/weekly.tsv (opened/closed per ISO week).

Everything here is a count over issue text (titles, labels, bodies): QUOTED evidence, not a
run. A cluster is a keyword match, so an issue can sit in several clusters and the cluster
sizes do not add up to the open count; `--exclusive` prints the first-match partition too.
"""
import argparse, collections, datetime as dt, json, os, re, statistics, sys

here = os.path.dirname(os.path.abspath(__file__))
data = os.path.join(here, "data")
outd = os.path.join(here, "out")
os.makedirs(outd, exist_ok=True)

ap = argparse.ArgumentParser()
ap.add_argument("--asof", default=None)
ap.add_argument("--exclusive", action="store_true")
args = ap.parse_args()

def load(name):
    return [json.loads(l) for l in open(os.path.join(data, name))]

open_ = load("issues-open.jsonl")
closed = load("issues-closed.jsonl")
allis = open_ + closed
asof = dt.date.fromisoformat(args.asof) if args.asof else max(
    dt.date.fromisoformat(i["updated_at"][:10]) for i in allis)
def d(s): return dt.date.fromisoformat(s[:10]) if s else None

print(f"# Tracker measurement, as of {asof} (data: {len(open_)} open, {len(closed)} closed, {len(allis)} issues)\n")

# ---------------------------------------------------------------- labels
lab = collections.Counter(l for i in open_ for l in i["labels"])
print("## Open issues by label (an issue carries several)\n")
for k, v in lab.most_common():
    print(f"{v:5d}  {k}")
print(f"{sum(1 for i in open_ if not i['labels']):5d}  (no label)")
print()

# ---------------------------------------------------------------- area
AREA_LABELS = ["sema", "codegen", "ownership", "async", "iterators", "cross-module", "stdlib",
               "parser", "runtime", "serialization", "test-infra", "rule-corpus", "documentation",
               "docs", "spec-tracking", "spec-question", "spec-defect", "spec", "enhancement", "RFC",
               "design-review", "ecosystem", "driver", "infra", "ratchet"]
TITLE_AREA = [
    ("test-infra", r"\[(test-infra|gate|harness|infra|corpus|ratchet)\]|\bgate\b|ratchet|harness|agent_check|pregate|lit track"),
    ("spec", r"^spec[- ]|spec-(question|defect)|\[spec\]|§\S+ \(v0\.\d+\)|\(v0\.\d+\):"),
    ("stdlib", r"\bstdlib\b|\[stdlib\]|Parse\b|TryParse|ToString|StringBuilder|Encoding|Math\.|DateTime|Guid"),
    ("codegen", r"\[codegen\]|\bcodegen\b|lower\w*|emit\w*|LLVM|IR verif|SIGSEGV|use-after-free|double-free|leak"),
    ("ownership", r"\[ownership\]|\bownership\b|borrow|dispos|owned|owning|move\b|§28|§30\.5"),
    ("sema", r"\[sema\]|\bsema\b|type ?check|overload|resolution|diagnostic|narrow"),
    ("parser", r"\[parser\]|\bparser\b|\blexer\b|grammar"),
    ("docs", r"\[docs?\]|guide|changelog|rationale"),
]
def area_of(i):
    labs = set(i["labels"])
    for a in AREA_LABELS:
        if a in labs:
            if a in ("documentation", "docs"): return "docs"
            if a in ("spec-question", "spec-defect", "spec"): return "spec"
            if a in ("design-review", "RFC", "enhancement"): return "enhancement/design"
            if a in ("infra", "ratchet", "rule-corpus"): return "test-infra"
            return a
    t = i["title"]
    for a, rx in TITLE_AREA:
        if re.search(rx, t, re.I): return a
    return "unclassified"
area = collections.Counter(area_of(i) for i in open_)
print("## Open issues by area (first matching component label, else title keywords)\n")
for k, v in area.most_common(): print(f"{v:5d}  {k}")
print()

# ---------------------------------------------------------------- severity
SEV_RX = r"use-after-free|UAF|double-free|double free|SIGSEGV|segfault|crash|ICE\b|hard-ICE|wild[- ]jump|IR verif|heap-buffer|over-release|wrong (code|value|answer|output)|silently|silent|miscompil|unsound|leak"
def severity(i):
    labs = set(i["labels"]); t = i["title"]
    if "soundness" in labs or "memory-safety" in labs: return "soundness"
    if "compiler-crash" in labs: return "crash"
    if "leak" in labs: return "leak"
    if re.search(r"use-after-free|UAF|double-free|double free|wild[- ]jump|heap-buffer|over-release|miscompil|unsound|wrong (code|value|answer|output)", t, re.I): return "soundness(title)"
    if re.search(r"SIGSEGV|segfault|crash|\bICE\b|hard-ICE|IR verif|hang|OOM", t, re.I): return "crash(title)"
    if re.search(r"\bleak", t, re.I): return "leak(title)"
    if re.search(r"silent", t, re.I): return "silent(title)"
    if re.search(r"accepts|over-accept|admits|compiles", t, re.I): return "over-accept(title)"
    if re.search(r"rejects|over-reject|refuse", t, re.I): return "over-reject(title)"
    return "other"
sev = collections.Counter(severity(i) for i in open_)
print("## Open issues by severity (label first, then title keywords)\n")
for k, v in sev.most_common(): print(f"{v:5d}  {k}")
print()

# ---------------------------------------------------------------- spec chapters in titles
chap = collections.Counter(); sect = collections.Counter(); stdl = collections.Counter()
for i in open_:
    for m in re.finditer(r"(stdlib\s+)?§\s*([0-9]+)(?:\.([0-9]+))?", i["title"]):
        if m.group(1):
            stdl[m.group(2) + ("." + m.group(3) if m.group(3) else "")] += 1
        else:
            chap[m.group(2)] += 1
            if m.group(3): sect[m.group(2) + "." + m.group(3)] += 1
print(f"## Spec chapters cited in open titles ({sum(chap.values())} citations in {sum(1 for i in open_ if '§' in i['title'])} titles)\n")
for k, v in sorted(chap.items(), key=lambda kv: -kv[1])[:30]: print(f"{v:5d}  §{k}")
print("\nTop sections:")
for k, v in sect.most_common(40): print(f"{v:5d}  §{k}")
print("\nstdlib sections:")
for k, v in stdl.most_common(15): print(f"{v:5d}  stdlib §{k}")
print()

# ---------------------------------------------------------------- age
def age(i): return (asof - d(i["created_at"])).days
buckets = [("<7d", 7), ("7-30d", 30), ("30-90d", 90), ("90-180d", 180), (">180d", 10**6)]
ab = collections.Counter()
for i in open_:
    a = age(i)
    for name, lim in buckets:
        if a < lim: ab[name] += 1; break
print("## Open issues by age (days since created)\n")
for name, _ in buckets: print(f"{ab[name]:5d}  {name}")
ages = sorted(age(i) for i in open_)
print(f"median age {statistics.median(ages)} d; oldest #{min(open_, key=lambda i: i['created_at'])['number']} ({ages[-1]} d)")
print()

# ---------------------------------------------------------------- self-declared siblings
SIB = [("sibling", r"\bsibling"), ("residual", r"\bresidual"), ("found while", r"found while"),
       ("pre-existing", r"pre-existing"), ("follow-up", r"follow-?up"), ("split from", r"split (out )?from|split of"),
       ("twin", r"\btwin\b"), ("same mechanism", r"same (mechanism|root cause|shape|site|family)")]
sib = collections.Counter(); sib_any = 0
for i in open_:
    text = i["title"] + "\n" + i["body"]
    hit = False
    for name, rx in SIB:
        if re.search(rx, text, re.I): sib[name] += 1; hit = True
    if hit: sib_any += 1
print("## Self-declared siblings / residuals (title or body; an issue can match several)\n")
for name, _ in SIB: print(f"{sib[name]:5d}  {name}")
print(f"{sib_any:5d}  any of the above ({100*sib_any/len(open_):.0f}% of open)")
ref = sum(1 for i in open_ if re.search(r"#\d{3,4}", i["title"]))
print(f"{ref:5d}  titles that name another issue number")
print()

# ---------------------------------------------------------------- spec-tracking
st = [i for i in open_ if "spec-tracking" in i["labels"]]
rev = collections.Counter(); prop = collections.Counter()
for i in st:
    for m in re.finditer(r"v0\.(\d{3})", i["title"]): rev["v0." + m.group(1)] += 1
    for m in re.finditer(r"(?:proposal\s*|\b)0(\d{3})\b", i["title"], re.I): prop["0" + m.group(1)] += 1
print(f"## spec-tracking trackers ({len(st)} open)\n")
print("By revision named in the title:")
for k, v in sorted(rev.items()): print(f"{v:5d}  {k}")
print("Proposals named in titles:", ", ".join(f"{k}×{v}" for k, v in prop.most_common(20)))
q = sum(1 for i in st if re.search(r"spec-question|\?$|does |is |should ", i["title"]))
print(f"{q:5d}  of them read as questions (title contains 'spec-question', a '?' or a modal)")
print()

# ---------------------------------------------------------------- opened vs closed per week
def week(dd): y, w, _ = dd.isocalendar(); return f"{y}-W{w:02d}"
opened = collections.Counter(week(d(i["created_at"])) for i in allis)
closedw = collections.Counter(week(d(i["closed_at"])) for i in closed if i["closed_at"])
weeks = sorted(set(opened) | set(closedw))
print("## Opened vs closed per ISO week (all issues; last 16 weeks)\n")
print("week        opened  closed   net   (open at week end, reconstructed)")
cum = 0
rows = []
for w in weeks:
    cum += opened[w] - closedw[w]
    rows.append((w, opened[w], closedw[w], opened[w] - closedw[w], cum))
for w, o, c, n, cum in rows[-16:]:
    print(f"{w}  {o:6d}  {c:6d}  {n:+5d}   {cum}")
with open(os.path.join(outd, "weekly.tsv"), "w") as f:
    f.write("week\topened\tclosed\tnet\topen_at_end\n")
    for r in rows: f.write("\t".join(map(str, r)) + "\n")
print()

# ---------------------------------------------------------------- topic clusters
CLUSTERS = [
    ("closures-captures", r"closure|lambda|captur|envdtor|boxed cell|capture cell|\bcell\b"),
    ("iterators-enumerables", r"iterator|yield|enumerat|enumerable|for-in|\bfor\b.*\bover\b|LINQ|pipeline|terminal\b|itmap"),
    ("async-tasks", r"\basync|await|\bTask\b|OwningTask|frame\b|Channel|TaskScope|Cancellation|WhenAll|WhenEach|resume"),
    ("disposal-scope-end", r"dispos|using var|destructor|scope[- ]end|drop-on-overwrite|reclaim|release|§28\.(3|9|10|11|12)"),
    ("owned-collections-literals", r"Owning(List|Dictionary|Array|Set|Stack|Queue|Buffer)|owning collection|owned collection|inline array|owning inline|collection literal|\[N\]|literal|Array<|Dictionary<|List<|Set<|\bSet\b|Stack<|Queue<"),
    ("nullable-narrowing", r"nullable|narrow|\?\.|\?\?|null-?conditional|\bT\?|§7\.(4|10)|null"),
    ("generics-instantiation", r"generic|instantiat|substitut|type (parameter|argument)|where T|monomorph|erased|template|§14\.|§3\.6"),
    ("string-format-parse", r"\bstring\b|ToString|\bParse\b|TryParse|format|StringBuilder|Encoding|interpolat|§2\.1[0-9]|stdlib §2|AppendFormat|render"),
    ("cross-module", r"cross-module|\bimport|\bmodule\b|metadata|--import|exported|dependency"),
    ("choice-match", r"\bchoice\b|owning match|payload|match arm|§35"),
    ("records-equality-hash", r"\brecord\b|Equals|GetHashCode|equality|hashable|\bhash\b|§9\.|§12\.5"),
    ("spans-borrows", r"\bspan\b|ReadOnlySpan|borrow|lifetime|escape|alias|§9\.7|§28\.(2|6|7|14)"),
    ("exceptions-unwind", r"throw|exception|\btry\b|catch|finally|unwind|§31"),
    ("contracts-pure", r"requires|ensures|invariant|contract|\bpure\b|§22\.1[56]"),
    ("extern-ffi", r"extern|\bFFI\b|\bPtr\b|unsafe|CString|WString|RawBytes|§32"),
    ("serialization", r"serializ|§33|\$case"),
    ("statics-init", r"\bstatic\b|initiali[sz]|§21"),
    ("interfaces-dispatch-extensions", r"interface|vtable|itable|dispatch|virtual|extension|§12\.|§26|§13"),
    ("numeric-ops", r"\bint\b|decimal|float|double|overflow|checked|shift|Math\.|integer|§2\.(2|3|4|5|6|7)|non-finite"),
    ("diagnostics-wording", r"diagnostic|message|wording|0:0|location|says|names the|tell"),
    ("parser-grammar", r"parser|lexer|grammar|token|Appendix A|\bA\.\d"),
    ("test-infra-gate", r"\[(test-infra|gate|harness|infra|corpus|ratchet)\]|\bgate\b|ratchet|harness|agent_check|pregate|corpus|ASan|runAot|bincache"),
]
def clusters_of(i):
    t = i["title"]
    cs = [n for n, rx in CLUSTERS if re.search(rx, t, re.I)]
    for l in i["labels"]:
        m = {"async": "async-tasks", "iterators": "iterators-enumerables", "cross-module": "cross-module",
             "serialization": "serialization", "parser": "parser-grammar", "test-infra": "test-infra-gate",
             "rule-corpus": "test-infra-gate"}.get(l)
        if m and m not in cs: cs.append(m)
    return cs
last30 = asof - dt.timedelta(days=30)
cl_open = collections.defaultdict(list)
for i in open_:
    for c in clusters_of(i): cl_open[c].append(i)
cl_closed30 = collections.Counter(); cl_closed_all = collections.Counter()
for i in closed:
    cs = clusters_of(i)
    for c in cs:
        cl_closed_all[c] += 1
        if i["closed_at"] and d(i["closed_at"]) >= last30: cl_closed30[c] += 1
print("## Topic clusters (title keyword match, overlapping)\n")
print("cluster                          open  sound  crash  leak  spec-trk  opened30d closed30d  closed-ever  medAge  bug-share")
rows = []
for name, _ in CLUSTERS:
    L = cl_open[name]
    if not L: continue
    s = sum(1 for i in L if severity(i).startswith("soundness"))
    c = sum(1 for i in L if severity(i).startswith("crash"))
    lk = sum(1 for i in L if severity(i).startswith("leak"))
    stk = sum(1 for i in L if "spec-tracking" in i["labels"])
    o30 = sum(1 for i in L if d(i["created_at"]) >= last30)
    med = statistics.median(age(i) for i in L)
    rows.append((name, len(L), s, c, lk, stk, o30, cl_closed30[name], cl_closed_all[name], med))
for r in sorted(rows, key=lambda r: -r[1]):
    name, n, s, c, lk, stk, o30, c30, ce, med = r
    print(f"{name:32s} {n:5d} {s:6d} {c:6d} {lk:5d} {stk:9d} {o30:10d} {c30:9d} {ce:12d} {med:7.0f}  {n/(n+ce):.2f}")
print("\n(bug-share = open / (open + closed-ever) for the cluster: how much of the cluster's history is still open)")
print()

if True:
    # exclusive partition: first matching cluster
    ex = collections.Counter((clusters_of(i) or ["(none)"])[0] for i in open_)
    print("## Exclusive partition (first matching cluster in table order)\n")
    for k, v in ex.most_common(): print(f"{v:5d}  {k}")
    print()

# ---------------------------------------------------------------- mechanisms named in titles
mech = collections.Counter()
for i in open_:
    for m in re.finditer(r"`?\b([a-z][a-zA-Z0-9]{5,}(?:::[a-zA-Z0-9]+)?)\(?`?", i["title"]):
        name = m.group(1)
        if re.search(r"[A-Z]", name) and name[0].islower():  # camelCase identifiers only
            mech[name] += 1
print("## camelCase identifiers (mechanisms) named in open titles, top 60\n")
for k, v in mech.most_common(60): print(f"{v:5d}  {k}")
print()
files = collections.Counter()
for i in open_:
    for m in re.finditer(r"\b([A-Za-z_]+\.(cpp|hpp))\b", i["title"] + " " + i["body"][:2000]):
        files[m.group(1)] += 1
print("## Source files named in open titles/bodies (first 2000 chars), top 40\n")
for k, v in files.most_common(40): print(f"{v:5d}  {k}")
print()

# ---------------------------------------------------------------- comments / body length
print("## Discussion volume\n")
cm = sorted(i["comments"] for i in open_)
print(f"comments per open issue: median {statistics.median(cm)}, mean {statistics.mean(cm):.2f}, {sum(1 for c in cm if c==0)} with none")
bl = sorted(i["body_len"] for i in open_)
print(f"body length: median {statistics.median(bl)} chars, {sum(1 for b in bl if b < 200)} under 200 chars")
print()

# ---------------------------------------------------------------- per-issue cluster table
with open(os.path.join(outd, "clusters.tsv"), "w") as f:
    f.write("number\tseverity\tage_days\tlabels\tclusters\ttitle\n")
    for i in sorted(open_, key=lambda i: i["number"]):
        f.write(f"{i['number']}\t{severity(i)}\t{age(i)}\t{','.join(i['labels'])}\t{','.join(clusters_of(i))}\t{i['title']}\n")

# ---------------------------------------------------------------- git landing rhythm
gl = os.path.join(data, "gitlog-files.txt")
if os.path.exists(gl):
    commits = []; cur = None
    for line in open(gl):
        line = line.rstrip("\n")
        if line.startswith("COMMIT|"):
            _, h, date, subj = line.split("|", 3); cur = {"h": h, "date": date, "subj": subj, "files": []}; commits.append(cur)
        elif line and cur is not None:
            cur["files"].append(line)
    def kind(s):
        if re.match(r"(fix|narrow|fix-forward)\b", s): return "fix"
        if re.match(r"(test|bank)\b", s): return "test/bank"
        if re.match(r"spec", s, re.I): return "spec"
        if re.match(r"(infra|scripts?|gate)\b", s, re.I): return "infra"
        if re.match(r"(docs?|harvest)\b", s, re.I): return "docs"
        return "other"
    def carea(c):
        a = set()
        for f in c["files"]:
            m = re.match(r"src/([a-z]+)/", f)
            if m: a.add(m.group(1))
            elif f.startswith("docs/"): a.add("docs")
            elif f.startswith("tests/"): a.add("tests")
            elif f.startswith("scripts/"): a.add("scripts")
        return a
    days = collections.defaultdict(collections.Counter); areas = collections.Counter(); fixissues = set()
    for c in commits:
        k = kind(c["subj"]); days[c["date"]][k] += 1
        if k == "fix":
            for m in re.finditer(r"#(\d+)", c["subj"]): fixissues.add(m.group(1))
            for a in carea(c):
                if a not in ("tests", "docs", "scripts"): areas[a] += 1
    active = [dd for dd in days if sum(days[dd].values()) >= 5]  # the shallow clone's first commits are stragglers
    first, last = min(active), max(active)
    span = (dt.date.fromisoformat(last) - dt.date.fromisoformat(first)).days + 1
    tot = collections.Counter()
    for dd in days: tot.update(days[dd])
    print(f"## Landing rhythm on origin/main, {first} .. {last} ({span} days, {len(commits)} commits)\n")
    print("kind        total  per-day")
    for k, v in tot.most_common(): print(f"{k:10s} {v:6d}  {v/span:6.1f}")
    print(f"\ndistinct issues named in fix/narrow subjects: {len(fixissues)}")
    print("\nfix commits touching src/<area> (a commit can touch several):")
    for k, v in areas.most_common(): print(f"{v:5d}  src/{k}")
    print("\nper day:")
    for dd in sorted(days): print(f"{dd}  " + "  ".join(f"{k}={v}" for k, v in sorted(days[dd].items())))
