#!/usr/bin/env python3
"""Reach measurements for proposal 0211 over the four program corpora (the corpora and
text preparation of catch_arms.py, which this script imports).

  1. Arms naming a straddling type, split by what the protected block calls:
     an `OverflowException` / `ArithmeticException` arm whose `try` block calls a
     `Parse` or `Convert.To*` member (the proposal's range-failure condition).
  2. Arms naming `DomainFaultedException`.
  3. Arms naming an interface type (an `I`-prefixed name) and whether they bind.
  4. Exception class declarations by base type, and which band the base puts them in
     under the proposal's taxonomy.
  5. Declarations and uses of the proposed names (`IFailure`, `ParseRangeException`,
     `DomainFailedException`) and of `Failure` (the declined base-class name), which
     must be free.
  6. Arms by the proposal's band table (catch-arm reach of the marker).
  7. `FormatException` arms split by what the protected block calls: a parse or decode
     member, or a format-specifier member (`ToString("...")`, `AppendFormat`).

Usage: band_reach.py <repo-root> <export-root> <out-dir>
"""
import collections, json, os, re, sys, importlib.util

here = os.path.dirname(os.path.abspath(__file__))
root, export, out = sys.argv[1:4]
spec = importlib.util.spec_from_file_location("catch_arms", os.path.join(here, "catch_arms.py"))
sys.argv = [sys.argv[0], root, export, os.path.join(out, "_tmp")]
ca = importlib.util.module_from_spec(spec)
import io, contextlib
with contextlib.redirect_stdout(io.StringIO()):
    spec.loader.exec_module(ca)

FAILURE = {"FormatException", "IOException", "FileNotFoundException", "DirectoryNotFoundException",
           "EndOfStreamException", "PathTooLongException", "TimeoutException", "SerializationException",
           "ChannelClosedException", "ChannelFullException"}
CANCEL = {"OperationCanceledException"}
DEFECT = {"ContractViolationException", "ArgumentException", "ArgumentNullException",
          "ArgumentOutOfRangeException", "ArithmeticException", "OverflowException", "DivideByZeroException",
          "InvalidOperationException", "DisposedAccessException", "NotImplementedException",
          "NullReferenceException", "IndexOutOfRangeException", "MatchFailureException",
          "TypeInitializationException", "KeyNotFoundException", "StackOverflowException"}
CARRIER = {"DomainFaultedException"}
def band(t):
    if t in FAILURE: return "failure"
    if t in CANCEL: return "cancellation"
    if t in DEFECT: return "defect"
    if t in CARRIER: return "carrier"
    if t == "Exception": return "catch-all"
    return "user-or-other"

PARSE_CALL = re.compile(r"\.Parse\s*\(|\bConvert\s*\.\s*To[A-Z]\w*\s*\(")
DECODE_CALL = re.compile(r"\.Parse\s*\(|\bConvert\s*\.\s*To[A-Z]\w*\s*\(|ParseOwned|\.Parse<|Deserialize|GetString\s*\(|GetBytes\s*\(|ReadAllText")
SPEC_CALL = re.compile(r"\.ToString\s*\(\s*\"|AppendFormat\s*\(")
DECL = re.compile(r"\bclass\s+(\w+)\s*(?:<[^>]*>)?\s*(?:\([^)]*\))?\s*:\s*([A-Z]\w*)")
res = collections.OrderedDict()
ovf = collections.Counter(); dom = collections.Counter(); iface = collections.Counter()
decl_base = collections.Counter(); names = collections.Counter(); arm_band = collections.Counter()
fmt = collections.Counter(); fmt_files = set()
ovf_files = collections.Counter()
user_exc = {}
all_texts = []
for name, files in ca.corpora().items():
    for kind, p in files:
        try: src = open(p, encoding="utf-8", errors="replace").read()
        except OSError: continue
        if kind == "cpp":
            src = ca.cpp_literals(src)
            if "module " not in src: continue
        text = ca.blank_bezel(src)
        all_texts.append((name, p, text))
        for m in DECL.finditer(text):
            user_exc.setdefault(m.group(1), m.group(2))

def is_exception_class(n, seen=()):
    if n == "Exception" or n in FAILURE or n in DEFECT or n in CANCEL or n in CARRIER: return True
    if n in seen or n not in user_exc: return False
    return is_exception_class(user_exc[n], seen + (n,))
def root_band(n, seen=()):
    if n in FAILURE: return "failure"
    if n in DEFECT: return "defect"
    if n in CANCEL: return "cancellation"
    if n in CARRIER: return "carrier"
    if n == "Exception": return "Exception (direct)"
    if n in seen or n not in user_exc: return "?"
    return root_band(user_exc[n], seen + (n,))

for name, p, text in all_texts:
    for m in DECL.finditer(text):
        if is_exception_class(m.group(2)):
            decl_base[(name, root_band(m.group(2)))] += 1
    for nm in ("IFailure", "ParseRangeException", "DomainFailedException", "Failure"):
        names[(nm, "declared")] += len(re.findall(r"\b(?:class|interface|record|enum|choice)\s+" + nm + r"\b", text))
        names[(nm, "mentioned")] += len(re.findall(r"\b" + nm + r"\b", text))
    for m in re.finditer(r"\bcatch\b", text):
        before = text[:m.start()]
        tries = [t.start() for t in re.finditer(r"\btry\b", before) if not re.search(r"\bend\s+$", before[:t.start()])]
        block = before[tries[-1]:] if tries else ""
        for idx, (pat, body) in enumerate(ca.parse_catch(text, m.end())):
            pm = ca.PATTERN.match(pat)
            if not pm: continue
            tl = [re.sub(r"<.*", "", t.strip()).split(".")[-1] for t in pm.group(2).split("|")]
            for t in tl:
                arm_band[(name, band(t))] += 1
                if t in ("OverflowException", "ArithmeticException"):
                    k = "try block calls Parse/Convert.To*" if PARSE_CALL.search(block) else "no Parse/Convert call in the try block"
                    ovf[(name, t, k)] += 1
                    if PARSE_CALL.search(block): ovf_files[os.path.relpath(p, root)] += 1
                if t == "FormatException":
                    k = ("parse or decode call" if DECODE_CALL.search(block) else
                         "format-specifier call" if SPEC_CALL.search(block) else "neither seen")
                    fmt[(name, k)] += 1
                    if k == "format-specifier call": fmt_files.add(os.path.relpath(p, root))
                if t == "DomainFaultedException":
                    dom[(name, ca.body_class(body))] += 1
                if re.match(r"I[A-Z]", t) and not is_exception_class(t):
                    iface[(name, t, "bind" if pm.group(1) else "bare")] += 1

def dump(c): return {" / ".join(map(str, k)): v for k, v in sorted(c.items())}
res["overflow_arms"] = dump(ovf)
res["overflow_arms_over_parse_total"] = sum(v for k, v in ovf.items() if k[2].startswith("try block calls"))
res["overflow_arms_total"] = sum(ovf.values())
res["overflow_over_parse_files"] = len(ovf_files)
res["domain_faulted_arms"] = dump(dom)
res["interface_typed_arms"] = dump(iface)
res["exception_class_declarations_by_root_band"] = dump(decl_base)
tot = collections.Counter()
for (n, b), v in decl_base.items(): tot[b] += v
res["exception_class_declarations_total_by_band"] = dict(tot)
res["proposed_names"] = dump(names)
res["format_arms"] = dump(fmt)
res["format_arms_over_specifier_calls"] = sum(v for k, v in fmt.items() if k[1] == "format-specifier call")
res["format_arms_over_specifier_files"] = len(fmt_files)
res["arms_by_band"] = dump(arm_band)
tot = collections.Counter()
for (n, b), v in arm_band.items(): tot[b] += v
res["arms_total_by_band"] = dict(tot)
os.makedirs(out, exist_ok=True)
json.dump(res, open(os.path.join(out, "band_reach.json"), "w"), indent=1)
with open(os.path.join(out, "overflow_over_parse_files.txt"), "w") as f:
    for p, n in sorted(ovf_files.items()): f.write(f"{n}\t{p}\n")
print(json.dumps(res, indent=1))
