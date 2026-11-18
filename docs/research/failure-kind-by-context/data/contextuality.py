#!/usr/bin/env python3
"""How the right handling of an exception type varies across the scenario sample.

Inputs (this directory): scenarios.tsv (first pass, with the three call-site features),
scenarios_pass2.tsv (second pass), conditions.tsv (condition -> Bezel class and v0.357 kind).

Labels:  L handle locally and continue        B drop the unit of work, report or retry it
         F fatal environment or configuration: stop; the code is right
         D a bug in the program: stop and fix the code
The brief's three bands are a = L, b = B, c = F or D.

Two readings of "failure vs defect" are scored against each type's v0.357 kind:
  owner's reading  failure <-> the program carries on (L, B); defect <-> it stops (F, D)
  the letter       failure <-> a correct program can meet it (L, B, F); defect <-> D
Rows whose type has no kind (cancellation, allocation failure, the domain-join carrier)
are left out of both scores.

failure_typed_stop_rows.tsv is a hand annotation of the failure-typed rows labelled F or D:
whether the operation runs at the top of the program (start-up, or a one-shot tool's main
line) or beneath a unit-of-work boundary whose handler answers routine failures; and, for
the second group, what could tell the row from the routine failures beside it: an arm on
its own class (`class`), a conversion at its own call (`call-site`), or only a test of the
cause, because the same call raises both with one class (`cause-value`).

Outputs: contextuality_by_type.tsv, contextuality_by_condition.tsv, results.json.
Usage: contextuality.py [dir]
"""
import collections, csv, json, math, os, sys

d = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
rd = lambda n: list(csv.DictReader(open(os.path.join(d, n), encoding="utf-8"), delimiter="\t"))
cond = {r["condition"]: r for r in rd("conditions.tsv")}
p2 = {r["id"]: r["pass2"] for r in rd("scenarios_pass2.tsv")}
rows = rd("scenarios.tsv")
for r in rows:
    c = cond[r["condition"]]
    r["type"], r["kind"], r["query"] = c["bezel_type"], c["kind_0211"].rstrip("*"), c["query_form"]
    r["pass2"] = p2[r["id"]]
    r["task_id"] = r["id"][:3]

BAND = {"L": "a", "B": "b", "F": "c", "D": "c"}

def kappa(xs, ys):
    n = len(xs); cats = sorted(set(xs) | set(ys))
    po = sum(1 for x, y in zip(xs, ys) if x == y) / n
    pe = sum((xs.count(c) / n) * (ys.count(c) / n) for c in cats)
    return round(po, 4), round((po - pe) / (1 - pe), 4)

def index(labels):
    """flip = 1 - share of the modal band; h = entropy over the three bands, normalised to 0..1"""
    n = len(labels); c = collections.Counter(BAND[x] for x in labels)
    flip = 1 - max(c.values()) / n
    h = -sum((v / n) * math.log(v / n) for v in c.values()) / math.log(3)
    return round(flip, 3), abs(round(h, 3))

def verdict(flip):
    return "stable" if flip <= 0.15 else ("leaning" if flip <= 0.40 else "contextual")

def table(key, fname):
    groups = collections.defaultdict(list)
    for r in rows: groups[r[key]].append(r)
    out = []
    for k, g in groups.items():
        l1 = [r["pass1"] for r in g]; l2 = [r["pass2"] for r in g]
        c1 = collections.Counter(l1)
        f1, h1 = index(l1); f2, _ = index(l2)
        nh = [r["pass1"] for r in g if r["operand_from"] != "hosted"]
        fnh = index(nh)[0] if nh else ""
        kind = g[0]["kind"]
        has_kind = kind in ("failure", "defect")
        owner = sum(1 for x in l1 if (x in "LB") == (kind == "failure")) if has_kind else ""
        letter = sum(1 for x in l1 if (x in "LBF") == (kind == "failure")) if has_kind else ""
        out.append([k, g[0]["type"], kind, len(g), len({r["task_id"] for r in g}),
                    c1["L"], c1["B"], c1["F"], c1["D"],
                    round((c1["L"] + c1["B"]) / len(g), 2), round(c1["D"] / len(g), 2), f1, h1, f2, fnh,
                    verdict(f1) if len(g) >= 5 else "(n<5)", owner, letter])
    out.sort(key=lambda x: (-x[3], x[0]))
    skip = 1 if key == "type" else None   # the type table needs no second type column
    with open(os.path.join(d, fname), "w", encoding="utf-8") as f:
        f.write("\t".join(x for i, x in enumerate([key, "bezel_type", "kind_v0357", "rows", "tasks", "L", "B", "F", "D",
                           "carries_on_share", "bug_share", "flip_pass1", "entropy_pass1", "flip_pass2", "flip_pass1_without_hosts",
                           "verdict", "fits_owner_reading", "fits_letter"]) if i != skip) + "\n")
        for o in out: f.write("\t".join(str(x) for i, x in enumerate(o) if i != skip) + "\n")
    return out

by_type = table("type", "contextuality_by_type.tsv")
by_cond = table("condition", "contextuality_by_condition.tsv")

R = collections.OrderedDict()
R["rows"] = len(rows); R["tasks"] = len({r["task_id"] for r in rows}); R["domains"] = len({r["domain"] for r in rows})
R["labels_pass1"] = dict(collections.Counter(r["pass1"] for r in rows))
R["labels_pass2"] = dict(collections.Counter(r["pass2"] for r in rows))
x1 = [r["pass1"] for r in rows]; x2 = [r["pass2"] for r in rows]
R["agreement_4_labels"] = dict(zip(["raw", "cohen_kappa"], kappa(x1, x2)))
R["agreement_3_bands"] = dict(zip(["raw", "cohen_kappa"], kappa([BAND[x] for x in x1], [BAND[x] for x in x2])))
R["disagreements"] = [{"id": r["id"], "pass1": r["pass1"], "pass2": r["pass2"]} for r in rows if r["pass1"] != r["pass2"]]

kinded = [r for r in rows if r["kind"] in ("failure", "defect")]
R["rows_with_a_kind"] = len(kinded)
R["rows_by_kind"] = dict(collections.Counter(r["kind"] for r in kinded))
def fit(p):
    o = sum(1 for r in kinded if (r[p] in "LB") == (r["kind"] == "failure"))
    l = sum(1 for r in kinded if (r[p] in "LBF") == (r["kind"] == "failure"))
    return {"owner_reading": o, "owner_reading_share": round(o / len(kinded), 3),
            "letter": l, "letter_share": round(l / len(kinded), 3)}
R["kind_fit_pass1"] = fit("pass1"); R["kind_fit_pass2"] = fit("pass2")
R["kind_by_label_pass1"] = {k: dict(collections.Counter(r["pass1"] for r in kinded if r["kind"] == k)) for k in ("failure", "defect")}

def ids(pred): return [r["id"] for r in kinded if pred(r)]
M = collections.OrderedDict()
M["failure_class_for_a_bug_(D)"] = ids(lambda r: r["kind"] == "failure" and r["pass1"] == "D")
M["failure_class_fatal_here_(F)"] = ids(lambda r: r["kind"] == "failure" and r["pass1"] == "F")
M["defect_class_expected_here_(L,B)_query_form_exists"] = ids(lambda r: r["kind"] == "defect" and r["pass1"] in "LB" and r["query"] == "yes")
M["defect_class_expected_here_(L,B)_tester_only"] = ids(lambda r: r["kind"] == "defect" and r["pass1"] in "LB" and r["query"] == "tester")
M["defect_class_expected_here_(L,B)_no_query_form"] = ids(lambda r: r["kind"] == "defect" and r["pass1"] in "LB" and r["query"] == "no")
M["defect_class_misconfiguration_(F)"] = ids(lambda r: r["kind"] == "defect" and r["pass1"] == "F")
R["mismatch_classes"] = {k: {"n": len(v), "of_which_hosted": sum(1 for r in kinded if r["id"] in v and r["operand_from"] == "hosted"), "ids": v} for k, v in M.items()}

# How well can any rule keyed on the type alone do? (resubstitution: an upper bound)
def majority_accuracy(key, lab):
    g = collections.defaultdict(list)
    for r in rows: g[r[key]].append(lab(r))
    return round(sum(max(collections.Counter(v).values()) for v in g.values()) / len(rows), 3)
R["best_rule_keyed_on_type_3_bands"] = majority_accuracy("type", lambda r: BAND[r["pass1"]])
R["best_rule_keyed_on_type_4_labels"] = majority_accuracy("type", lambda r: r["pass1"])
R["best_rule_keyed_on_condition_3_bands"] = majority_accuracy("condition", lambda r: BAND[r["pass1"]])
R["best_rule_keyed_on_kind_3_bands"] = majority_accuracy("kind", lambda r: BAND[r["pass1"]])
R["best_rule_keyed_on_task_3_bands"] = majority_accuracy("task_id", lambda r: BAND[r["pass1"]])
R["best_rule_keyed_on_operand_from_3_bands"] = majority_accuracy("operand_from", lambda r: BAND[r["pass1"]])
R["best_rule_keyed_on_type_3_bands_pass2"] = majority_accuracy("type", lambda r: BAND[r["pass2"]])
R["failure_typed_rows_handled_at_the_call_(L)_by_query_form"] = dict(collections.Counter(
    r["query"] for r in kinded if r["kind"] == "failure" and r["pass1"] == "L"))

# The three call-site questions: is there a fallback? who supplied the operand? what stops?
def rule(r):
    if r["fallback"] == "y": return "L"
    if r["operand_from"] == "program": return "D"
    if r["scope"] == "program": return "F"
    return "B"
for p in ("pass1", "pass2"):
    R[f"three_question_rule_4_labels_{p}"] = round(sum(1 for r in rows if rule(r) == r[p]) / len(rows), 3)
R["three_question_rule_misses_pass1"] = [f"{r['id']}:{rule(r)}->{r['pass1']}" for r in rows if rule(r) != r["pass1"]]

T = [o for o in by_type if o[3] >= 5]
R["types_with_5_or_more_rows"] = {v: [o[0] for o in T if o[15] == v] for v in ("stable", "leaning", "contextual")}
R["types_with_5_or_more_rows_pass2"] = {v: [o[0] for o in T if verdict(o[13]) == v] for v in ("stable", "leaning", "contextual")}

# The two binary readings, per type (types with a kind and 5 or more rows): a type is "mixed"
# when its minority side holds at least 15% of its rows.
def mixed(share): return 0.15 <= share <= 0.85
K = [o for o in T if o[2] in ("failure", "defect")]
R["owner_reading_by_type"] = {"mixed": [o[0] for o in K if mixed(o[9])], "one_sided": [o[0] for o in K if not mixed(o[9])]}
R["letter_by_type"] = {"mixed": [o[0] for o in K if mixed(o[10])], "one_sided": [o[0] for o in K if not mixed(o[10])]}

# The reading most favourable to the landed text: leave out hosted rows (a host's catch-all is
# the sanctioned arm) and every defect-typed row whose condition has a query form or a tester
# (a well-written Bezel program takes that form and nothing is raised); score the rest on the
# letter. What remains unfitted cannot be removed by reading the rows more kindly.
fav = [r for r in kinded if r["operand_from"] != "hosted"
       and not (r["kind"] == "defect" and r["pass1"] != "D" and r["query"] in ("yes", "tester"))]
unfit = [r for r in fav if (r["pass1"] in "LBF") != (r["kind"] == "failure")]
R["most_favourable_reading"] = collections.OrderedDict([
    ("rows_scored", len(fav)), ("unfitted", len(unfit)), ("unfitted_share", round(len(unfit) / len(fav), 3)),
    ("failure_class_for_a_bug", [r["id"] for r in unfit if r["kind"] == "failure"]),
    ("defect_class_for_a_condition_with_no_query_form", [r["id"] for r in unfit if r["kind"] == "defect"])])

# A boundary arm on the marker, placed at the unit-of-work boundary.
where = {r["id"]: r["where_raised"] for r in rd("failure_typed_stop_rows.tsv")}
stop = [r for r in kinded if r["kind"] == "failure" and r["pass1"] in "FD"]
assert {r["id"] for r in stop} == set(where), "failure_typed_stop_rows.tsv is out of step with scenarios.tsv"
B = [r for r in kinded if r["pass1"] == "B"]
A = collections.OrderedDict()
A["rows_a_boundary_should_answer_(B)"] = len(B)
A["of_those_failure_typed_(the_arm_takes_them)"] = sum(1 for r in B if r["kind"] == "failure")
A["of_those_defect_typed_(the_arm_passes_them)"] = [r["id"] for r in B if r["kind"] == "defect"]
A["failure_typed_rows_labelled_F_or_D"] = len(stop)
A["of_those_raised_at_the_top_(no_unit_boundary_above)"] = sum(1 for r in stop if where[r["id"]] == "top")
A["of_those_raised_under_a_unit_boundary_(the_arm_answers_them_as_routine)"] = [r["id"] for r in stop if where[r["id"]] == "under-unit"]
A["those_by_what_separates_them"] = dict(collections.Counter(
    r["separable_by"] for r in rd("failure_typed_stop_rows.tsv") if r["where_raised"] == "under-unit"))
R["marker_arm_at_the_unit_boundary"] = A
json.dump(R, open(os.path.join(d, "results.json"), "w"), indent=1)
print(json.dumps({k: v for k, v in R.items() if k in ("most_favourable_reading", "owner_reading_by_type", "letter_by_type")}, indent=1))
for k, v in R["mismatch_classes"].items(): print(k, v["n"], "hosted:", v["of_which_hosted"])
print("rule misses:", R["three_question_rule_misses_pass1"])
