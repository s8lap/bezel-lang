#!/usr/bin/env python3
"""Assign every open issue to at most one campaign (first match wins, in the order below) and
print the member lists the master plan quotes. Re-run after `fetch_issues.sh` to refresh.

The rules are title/label regexes plus explicit seed numbers read from the issue bodies while the
plan was written. They are a triage aid, not a verdict: a campaign's ENTRY check re-measures every
member on HEAD and may move or split it (master-plan.md §2). Everything here is QUOTED evidence.

Usage: python3 -I campaigns.py [--tsv] > out/campaign-members.md
"""
import collections, datetime as dt, json, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
open_ = [json.loads(l) for l in open(os.path.join(here, "data", "issues-open.jsonl"))]
closed = [json.loads(l) for l in open(os.path.join(here, "data", "issues-closed.jsonl"))]
asof = max(dt.date.fromisoformat(i["updated_at"][:10]) for i in open_ + closed)

# (slug, title, sections, title-regex, label-regex, seeds)
CAMPAIGNS = [
    ("infra-oracles", "Enablers: oracles, ratchets, gate and silent degrade",
     "tests/README, §22.3 internal:",
     r"\[(test-infra|gate|harness|infra|corpus|ratchet|lit|build|toolchain|tests?|testing|seam-debt|gate-perf)\]|^(tests?|gate|lit|conformance|rule corpus|check_\w+|agent_check|pregate|ratchet|infra)\b|internal:|degrade|vacuous|cannot fail|leak census|oracle|runAot|bincache|worktree|\bgate\b",
     r"test-infra|rule-corpus|ratchet|infra",
     [2924, 7145, 4452, 3917, 4383, 6364, 7069, 7070, 7071, 7065, 6384, 7236, 7335, 4602]),
    ("using-heads", "`using var` / `await using` over every head kind",
     "§28.10, §28.12, §30.7, stdlib §2.10",
     r"await using|using var .*(string|lambda|function|choice|copy|generic type parameter|interface|abstract)|DisposeAsync|§28\.12|IAsyncDisposable",
     r"$^",
     [7322, 7327, 7283, 7306, 7316, 7305, 7357, 3493, 3483, 3516, 3515, 3482, 1603, 1531, 2623, 2624, 3471, 5653, 7354, 6194]),
    ("enumerable-heads", "Enumerable heads and iterator frames: one bridge per face",
     "§29.2, §29.11, §28.6, §21.4, §21.5, stdlib §5",
     r"IEnumerable|IOwningEnumer|enumerable|enumerator|iterator|iterable|\byield\b|wrapper frame|§29\.|§21\.4|§21\.5|for-in|\bfor\b.*\b(over|in)\b|lazy|LINQ|pipeline|terminal|ToArray|GetEnumerator|getter.*(frame|UAF|SEGV)|frame.*(getter|launder)|itmap|Select|GroupBy|Distinct|OrderBy|materiali[sz]",
     r"iterators",
     [3469, 3370, 3519, 3522, 1776, 3479, 3478, 3476, 3475, 2297, 1839, 2307, 3363, 3361, 3374, 3561, 3544, 3563, 2520, 3406, 3390,
      6453, 6455, 6406, 6409, 6412, 6387, 6393, 6396, 6447, 6451, 6489, 6493, 6539, 6545, 6563, 6567, 6613, 6185, 6186, 6242, 6245, 6277,
      7063, 7160, 7179, 7183, 7184, 7212, 7213, 6403, 6407, 6487, 6490, 6491, 4827, 4828, 4886, 4887, 4888, 4007, 4009, 4010, 6356, 6360, 6964, 6863, 6783, 6819, 6890]),
    ("producer-temporaries", "Producer temporaries and boxes: one discharge authority",
     "§28.3 (K8, K8.2, producers row), §28.9, §8.6, §17.13",
     r"temporar|unbound|never (discharged|disposed|released|reclaimed|freed)|not (discharged|disposed|released|reclaimed)|discharg|\+1|producer|fresh\b|\bbox(ed|es)?\b|§8\.6|receiver temp|interpolation hole|while CONDITION|discard|_ :=|bare expression statement|leaks? (its|the|one|a|every|35|24|16|8|4)|leak\b",
     r"leak",
     [7152, 7171, 7187, 7188, 7211, 7230, 7231, 7267, 7272, 7222, 6775, 6632, 6889, 6896, 6769, 6827, 6149, 4841, 6481, 6457, 6366, 6374, 6418, 7190, 6736, 6744, 6857, 7278, 6938,
      6421, 6444, 5350, 6003, 6117, 6633, 7005, 7298, 6929, 6926, 6733, 6209, 6093, 6094, 6085, 6930, 5951, 4782, 6832, 3447, 3480, 2760, 3908, 3905, 3904, 2371, 3357, 7312, 7310]),
    ("builtin-call-admission", "Built-in call admission: typed arms for the stdlib surface",
     "§22.6, §5.1, §7.4, stdlib §2, §3, §9",
     r"Parse\b|TryParse|ToString|AppendFormat|StringBuilder|Convert\.|Math\.|string\.(Join|Remove|Equals|Contains|Split|Replace|IndexOf|LastIndexOf)|Encoding|format[- ]specifier|radix|non-string first argument|malformed IR|IR verif|aborts? the compiler|ICE\b|builtin|built-in (non-string|collection interface|static|member)|name-keyed|by (HEAD )?name|head name|isStrBld|nullable .*(Math|argument|box)|§7\.4-narrowed|narrowed.*(carrier|unwrap)|carrier",
     r"stdlib|compiler-crash",
     [7244, 7285, 7290, 7291, 7293, 7294, 7299, 7300, 7239, 7265, 7284, 7358, 6809, 7180, 6635, 6636, 7217, 6845, 7154, 7274, 7297, 7232, 6823, 6255, 6957, 6648, 6864, 6865, 6782, 7108, 7189, 7210, 7229, 6637, 2625, 2642, 7004, 7089, 7066, 6659, 6677, 6084, 6116, 6126, 2620, 6254, 6750, 6751, 6942, 6295]),
    ("closures-cells", "Closures and capture cells, second pass",
     "§27.5, §28.9, §28.14, §28.3 sink rule",
     r"closure|lambda|captur|envdtor|capture cell|\bcell\b|method reference|function[- ]typed|function value|§27\.5|§28\.14|sink rule|call-bounded|scoped callback",
     r"$^",
     [7288, 7112, 7259, 6953, 7031, 6766, 6839, 6628, 6480, 6212, 6213, 6226, 6230, 6269, 6306, 5927, 6108, 6127, 6137, 5850, 5851, 5866, 5228, 4938, 4860, 4534, 4535, 4542, 4522, 4474, 4431, 4432, 4214, 4230, 4130, 4031, 3403, 3466, 2622, 3928, 3940, 4433, 4438,
      2225, 2248, 2249, 2383, 2578, 2626, 2640, 2715, 2719, 3459, 3500, 3518, 3532, 3541, 3824, 4425, 6634, 6651, 6652, 6653, 6710, 6696, 7038, 6141, 6061, 4723, 5258, 5259, 6287, 6293]),
    ("collection-elements", "Owned collections: element lifetime in the runtime",
     "§15.4, §28.11, §28.7, stdlib §4",
     r"Owning(List|Dictionary|Array|Set|Stack|Queue|Buffer|LinkedList)|Array<|Dictionary<|Set<|Stack<|Queue<|LinkedList|SortedDictionary|FrozenArray|elem_disposer|element[s]? (leak|disposer|never)|collection literal|inline array|\[N\]|bz_dict|bz_array|string elements|its strings|key snapshot|Slice",
     r"$^",
     [6936, 6937, 7058, 7059, 7060, 6945, 6992, 6993, 6994, 6691, 6238, 6282, 6145, 6358, 6943, 2648, 2645, 6516, 6514, 7216, 6672, 6685, 6760, 6995, 7052, 7053, 7043, 2327, 3393, 4332, 2762, 2676, 2672, 2685, 6184, 6448, 6467, 6365, 6488, 7076, 7081, 7082, 6314, 7240, 7092, 4096, 4427, 4351, 3465]),
    ("nullable-carrier", "Nullable carrier unwrap at every value consumer",
     "§7.4, §7.5, §7.7, §7.10, §5.1, §12.7, §19.5",
     r"nullable|narrow|\?\?|\?\.|null-?conditional|\bT\?|int\?|double\?|§7\.(4|5|7|10)|§12\.7|§19\.5|null\b",
     r"$^",
     [6072, 6073, 6225, 6859, 6482, 6505, 5903, 5522, 6731, 6723, 6724, 7350, 7353, 6772]),
    ("generic-boundaries", "Generic instantiation boundaries",
     "§14.1, §14.2, §14.5, §3.6, §12.9, §28.3 bare-type-parameter rules",
     r"generic|instantiat|substitut|type parameter|type argument|bare[- ]T\b|\bT\b.*(template|placeholder)|monomorph|erased|template|§14\.|§3\.6|where T",
     r"$^",
     [7021, 7056, 6792, 6931, 6932, 6927, 6257, 6254, 6232, 6112, 6021, 4861, 4862, 2289, 2291, 3348, 3378, 3524, 3655, 7062, 7066, 6866, 6747, 6315, 4980, 5417, 6392, 6443, 6407,
      710, 2020, 2220, 2650, 2861, 3526, 3659, 2580, 3474, 3485, 3486, 3495, 3501, 3514, 3525, 2735, 2736, 4567, 4568, 6295, 2755, 3566]),
    ("borrow-provenance", "Borrow provenance and the sink rule: ownership-pass catch-up",
     "§28.3 provenance/consume-site/freeze, §28.14, §9.7, §28.2, §28.6, §28.7, §30.5",
     r"borrow|provenance|holds-borrows|borrow-bearing|escape|alias|lifetime|§9\.7|§28\.(2|3|6|7|14)|§30\.5|consume|freeze|checkRead|markMoved|Disposed|moved-from|transfer window|drop-on-overwrite|displace|reassign",
     r"ownership",
     [7107, 7097, 7240, 7092, 6886, 5973, 5974, 5845, 6136, 6616, 6708, 6737, 7087, 7075, 6848, 3967, 4102, 4122, 4124, 3979, 3959, 3999, 4055, 4450, 4023, 4428, 3827, 3520, 3569, 2215, 2255, 4429, 4231, 3709, 2745, 2749, 4053, 3873, 4262, 4215, 1772,
      4301, 4277, 4274, 4291, 4360, 2615, 2636, 2705, 2690, 1933, 2577, 2569, 2551, 2570, 3632, 2671, 2740, 6977, 7009, 6288, 5880, 5855, 6153, 4774, 6867]),
    ("choice-payloads", "Choice payload lifetime and layout",
     "§35, §19.3, §15.3, §9.4",
     r"\bchoice\b|owning match|payload|match arm|§35|Result<|case this",
     r"$^",
     [7316, 7357, 7305, 7047, 6738, 6170, 6233, 5912, 5992, 6014, 6168, 6167, 5453, 5153, 4804, 4684, 4634, 6164, 6165, 6084, 6232, 6478, 3905, 2220, 6034, 6043, 6638, 6640, 4816, 4883, 3939, 2571]),
    ("async-frames", "Async return slot and frame-carried locals",
     "§30.5, §30.1, §30.9, §30.10, stdlib §6",
     r"\basync|await|\bTask\b|OwningTask|TaskScope|Channel|Cancellation|\$Resume|suspension|continuation",
     r"async",
     [6097, 6098, 6099, 6051, 5949, 5311, 5280, 5276, 5410, 5982, 5981, 6194, 6211, 6245, 4883, 5015, 3460, 1422, 6173, 6446, 5831, 5699, 5704, 5719, 5722, 5657, 5900]),
    ("cross-module", "Cross-module parity: retire AST-gated symbol reads",
     "§2.5, §2.8, §22.10 (imported), check_imported_symbol_reads",
     r"cross-module|\bimport|imported|\bmodule\b|artifact|--import|xmod",
     r"cross-module",
     [7223, 7062, 7078, 6943, 6944, 6991, 6994, 7008, 6743, 6833, 6918, 6927, 6932, 6364, 6360, 6206, 6257, 6289, 6043, 5214, 3671, 3655, 3378, 3357, 3350, 2861, 2648, 2650, 2735, 2291, 1425, 7281, 2590]),
    ("records-statics-misc", "Records, statics, contracts, exceptions and the remainder",
     "§8.4, §9.x, §21, §22.8, §22.16, §31",
     r"record|static|readonly|invariant|requires|ensures|contract|\bpure\b|exception|\bthrow|catch|finally|§31|§21|§22\.(8|16)|§8\.4|§9\.",
     r"$^",
     [6204, 6206, 6497, 6498, 4645, 6856, 6857, 6858, 7057, 2295, 2114, 3966, 1882, 1878]),
    ("spec-trains", "Spec-tracking trackers not yet claimed by a campaign (triage at each train)",
     "per tracker", r"$^", r"spec-tracking", []),
    ("features-proposals", "Feature and proposal trackers (not defects)",
     "docs/proposals", r"\[proposal|^Proposal|Windows build|release artifact|ecosystem", r"enhancement|RFC|design-review|ecosystem|documentation", []),
]

seed_index = {}
for slug, *_, seeds in [(c[0],) + c[1:] for c in CAMPAIGNS]:
    for s in seeds:
        seed_index.setdefault(s, slug)

def assign(i):
    if i["number"] in seed_index:
        return seed_index[i["number"]]
    labs = " ".join(i["labels"])
    t = i["title"]
    for slug, title, secs, trx, lrx, seeds in CAMPAIGNS:
        if slug in ("infra-oracles",) and re.search(trx, t, re.I): return slug
        if re.search(lrx, labs) and slug in ("infra-oracles", "features-proposals", "spec-trains"): return slug
    for slug, title, secs, trx, lrx, seeds in CAMPAIGNS:
        if slug in ("infra-oracles", "features-proposals", "spec-trains"): continue
        if re.search(trx, t, re.I) or (lrx != r"$^" and re.search(lrx, labs)):
            return slug
    return "unassigned"

def severity(i):
    labs = set(i["labels"]); t = i["title"]
    if "soundness" in labs or "memory-safety" in labs or re.search(r"use-after-free|UAF|double-free|wild[- ]jump|heap-buffer|over-release|miscompil|unsound|wrong (code|value|answer|output)", t, re.I): return "soundness"
    if "compiler-crash" in labs or re.search(r"SIGSEGV|segfault|crash|\bICE\b|IR verif|hang|OOM|malformed IR|aborts? the compiler", t, re.I): return "crash"
    if "leak" in labs or re.search(r"\bleak", t, re.I): return "leak"
    return "other"

by = collections.defaultdict(list)
for i in open_:
    by[assign(i)].append(i)
closed_by = collections.Counter()
last30 = asof - dt.timedelta(days=30)
closed30_by = collections.Counter()
for i in closed:
    s = assign(i); closed_by[s] += 1
    if i["closed_at"] and dt.date.fromisoformat(i["closed_at"][:10]) >= last30: closed30_by[s] += 1

if "--tsv" in sys.argv:
    print("number\tcampaign\tseverity\tseed\tlabels\ttitle")
    for i in sorted(open_, key=lambda i: i["number"]):
        print(f"{i['number']}\t{assign(i)}\t{severity(i)}\t{'seed' if i['number'] in seed_index else 'rule'}\t{','.join(i['labels'])}\t{i['title']}")
    sys.exit()

print(f"# Campaign assignment of the {len(open_)} open issues (as of {asof}; exclusive, first match wins)\n")
print("| campaign | open | soundness | crash | leak | spec-trk | opened 30d | closed 30d | closed ever | seeds |")
print("|---|---|---|---|---|---|---|---|---|---|")
order = [c[0] for c in CAMPAIGNS] + ["unassigned"]
for slug in order:
    L = by.get(slug, [])
    sev = collections.Counter(severity(i) for i in L)
    stk = sum(1 for i in L if "spec-tracking" in i["labels"])
    o30 = sum(1 for i in L if dt.date.fromisoformat(i["created_at"][:10]) >= last30)
    seeds = sum(1 for i in L if i["number"] in seed_index)
    print(f"| {slug} | {len(L)} | {sev['soundness']} | {sev['crash']} | {sev['leak']} | {stk} | {o30} | {closed30_by[slug]} | {closed_by[slug]} | {seeds} |")
print()
for slug, title, secs, *_ in CAMPAIGNS:
    L = sorted(by.get(slug, []), key=lambda i: (severity(i) != "soundness", severity(i) != "crash", i["number"]))
    print(f"\n## {slug} — {title} ({len(L)} open; {secs})\n")
    for i in L:
        age = (asof - dt.date.fromisoformat(i["created_at"][:10])).days
        mark = "S" if i["number"] in seed_index else " "
        print(f"- #{i['number']} [{severity(i)[:5]}] {mark} ({age}d) {i['title'][:150]}")
L = by.get("unassigned", [])
print(f"\n## unassigned ({len(L)})\n")
for i in sorted(L, key=lambda i: i["number"]):
    print(f"- #{i['number']} [{severity(i)[:5]}] {i['title'][:150]}")
