// Spec-derived regression tests for fix #3865: `SortedSet<T>.Contains`/
// `.First`/etc. over a record element with NO `CompareTo` at all (and the
// `SortedDictionary<K, V>` KEY analogue this issue's own sweep found) used to
// emit malformed IR or silently zero-degrade, reachable through #3490's still-
// open sema gap ("nothing checks stdlib's `where T: IComparable<T>` /
// `where K: IComparable<K>` constraint for a builtin generic").
//
// AUTHORITY. stdlib §4.18 (v0.294) declares, verbatim:
//
//     public class SortedSet<T>: IReadOnlyCollection<T> where T: IComparable<T>
//
// under the heading sentence "A comparison-ordered set: membership by
// `CompareTo`-equality, iteration in ascending order." stdlib §4.17 declares
// the identical shape for the dictionary form:
//
//     public class SortedDictionary<K, V>: IReadOnlyCollection<DictionaryEntry<K, V>>
//         where K: IComparable<K>
//
// THE #3865 DEFECT (measured on 2760e31, the fix #3918 HEAD this lane started
// from). A record `NoCmp` that declares an explicit field (so it is not
// auto-hashable per §9.2) and NO `CompareTo` at all does not satisfy either
// constraint, but nothing rejects a BORROWED `SortedSet<NoCmp>` /
// `SortedDictionary<NoCmp, V>` parameter — #3490's sema gap. Reaching
// `.Contains`/`.Remove` on such a receiver produced a genuine `bezelc` abort:
//
//     Invalid operand types for ICmp instruction
//       %14 = icmp eq %record.NoCmp %13, %fst.r
//     bezelc: codegen error: IR verification failed (malformed IR emitted...)
//
// because `genericBackingType` (LlvmBackend_Calls3.cpp) correctly declined
// the element (returning null), but the SET-family call-lowering
// (`tryLowerCollectionInstanceCall`, LlvmBackend_Calls.cpp) then silently fell
// through to `Set<T>`/`OwningSet<T>`'s ARRAY-backed fallback arms — sound for
// those two receivers (whose runtime really is a `bz_array_t*` for a
// non-eligible element, #1070), but never sound for `SortedSet`, whose
// runtime is ALWAYS a `bz_dict_t*` (narrow #3428's own "SortedSet is
// hash-backed or it is nothing"). `.First`/`.Last` alone (not combined with a
// consumer) silently returned a garbage struct read from the wrong layout,
// with no diagnostic at all.
//
// This lane's own sweep (per the issue's "sweep the whole family" brief)
// found the IDENTICAL root cause on the `SortedDictionary<K, V>` KEY side, in
// a DIFFERENT, adjacent codegen block: `genericBackingType(isSet=false)`
// returning null there is silently treated as "the key is `string`" (the
// `isDict` block's own `!keyTy` convention, correct for `Dictionary` /
// `OwningDictionary`), producing a DIFFERENT malformed-IR pattern — a
// `%record.T` struct fed into `%bz.string`-shaped GEP/load pairs:
//
//     GEP base pointer is not a vector or a vector of pointers
//       %12 = getelementptr inbounds nuw %bz.string, %record.NoCmp %11, ...
//
// — for a record with no `CompareTo` at all (illegal per §4.17); the
// DISTINCT legal-`CompareTo` shape (a record that DOES declare a dispatchable
// `CompareTo`, which fix #3740 admitted for `SortedSet<T>`'s ELEMENT but had
// not extended to `SortedDictionary<K, V>`'s KEY, since that exception was
// gated on `isSet` inside `genericBackingType`) was tracked as a follow-up
// issue from #3865's own sweep, fixed by #3948, and now exercises a full
// functional surface in sorteddictionary_cmp_key_3948_test.cpp — that file
// is the regression test that pins the record-`CompareTo` KEY analogue
// `SortedSet<T>`'s ELEMENT analogue got from #3740.
//
// THE FIX. `tryLowerCollectionInstanceCall`'s `SortedSet` arm and its
// `isDict` block's `SortedDictionary` handling both now report ONE located,
// spec-cited diagnostic (LlvmBackend_Calls.cpp) instead of falling through to
// the mismatched backing; the two PROPERTY-syntax twins (`s.First` / `.Last`,
// `d.FirstKey` / `.LastKey`, parsed as a bare `MemberAccessExpr` rather than
// a `CallExpr` — LlvmBackend_Exprs.cpp) get the identical treatment, closing
// what was otherwise a silent #2434 zero-degrade with NO diagnostic there.
//
// REVIEW FINDING / fix #3956 (folded into this same commit). The FIRST cut of
// this fix added only ONE guard per file half — the `!elemTy`/`!keyTy` early
// decline, which fires when `genericBackingType` returns null (a record that
// is neither auto-hashable nor `CompareTo`-dispatchable, or a nullable
// element). That guard structurally CANNOT catch a class/interface element,
// or an AUTO-HASHABLE record element (issue 3947's own shape), with no
// `CompareTo`: `genericBackingType` returns NON-null for both (via
// `hasObjectHeaderEquality` / `recordTypeOfRef`), so `elemTy`/`keyTy` is
// truthy and the ELIGIBLE branch runs instead — reaching a PRE-EXISTING,
// separate `if (!cmpFn) return nullptr;` (`buildSortedKeyComparator` returns
// null for a `CompareTo`-less element even when eligible) that #3865's first
// cut left untouched in `LlvmBackend_Calls.cpp`'s CALL-syntax arms
// (`s.First()`/`.Last()`, `d.FirstKey()`/`.LastKey()`), even though the
// PROPERTY-syntax twins in `LlvmBackend_Exprs.cpp` already had the guard.
// MEASURED: `s.First()` (call syntax, class OR auto-hashable-record element,
// no `CompareTo`) silently degraded to the #2434 zero-fill (exit 0, WRONG),
// while `s.First` (property syntax, identical receiver) already produced the
// located diagnostic (exit 1, correct) — the MORE NATURAL spelling was the
// one still broken. Fixed by mirroring the exact `!cmpFn` diagnostic from
// `LlvmBackend_Exprs.cpp` into the two call-syntax sites.
//
// A rejection is asserted on the diagnostic TEXT, never via a coarse
// `hasErrors()` or a bare "did not run": `runAot`'s WIFSIGNALED path
// synthesizes an `Error` diagnostic for a crash too, so only a diagnostic
// carrying the SPECIFIC needle text distinguishes a genuine, located
// rejection from the very crash this fix removes.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The VALUE channel for an observable outside 0..255 (WEXITSTATUS truncates).
std::string out(const std::string& src) { return runAotOut(src); }

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// True when NO diagnostic in the compile carries `needle` (used to guard
// against a false claim creeping into a message, mirroring #3428's own
// `nullable_class_element_diagnoses_nullability` pattern).
bool lacks(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return false;
    return true;
}

// `NoCmp`: an explicit-field record (§9.2: not auto-hashable) with NO
// `CompareTo` at all — the exact shape #3865 is filed against. `Cmp`: the
// identical shape but WITH a dispatchable single-parameter `CompareTo` — the
// #3740-admitted, still-legal §4.17/§4.18 control. `Ord`: a class satisfying
// `IComparable<Ord>`, the untouched `Set<T>`/`OwningSet<T>` false-reject
// control (mirrors sorted_set_class_element_3428_test.cpp's own `Ord`).
//
// `Cmp` LISTS `IComparable<Cmp>`, and that listing is what makes it conform
// under spec §14.2 rule 2: "T is a record that lists I (or any interface
// extending I) in its inheritance clause AND declares matching members". A
// record with a `CompareTo` but no listing satisfies neither half, so
// fix #4318's `where T: IComparable` check rejects `SortedSet<Cmp>` at sema
// time (v0.315) — correctly, and with no lowering run at all. The listing was
// added to this fixture for that reason: the rows using `Cmp` are ACCEPT rows
// whose real subject is the ordering/dedup BEHAVIOUR #3740/#3948 implemented,
// so they must keep a conforming program and keep asserting those values.
// (fix #3740's own regression file, sorted_set_record_element_3740_test.cpp,
// spells its records this way too, so its 11 rows are unaffected.)
//
// `ClassNoCmp` / `AutoHashRec`: the review-fix #3956 shapes. Both are
// `genericBackingType`-ELIGIBLE (`ClassNoCmp` via `hasObjectHeaderEquality`,
// `AutoHashRec` via `recordTypeOfRef` — an auto-property-only record IS
// auto-hashable per §9.2) but declare NO `CompareTo` at all, so
// `buildSortedKeyComparator` still declines them — the shape #3865's FIRST
// cut's `!elemTy`/`!keyTy` guard could not reach (that guard fires only when
// `genericBackingType` returns null, which it does NOT for either of these).
const char* PRELUDE =
    "module M; "
    "record NoCmp "
    "  public Item: int; "
    "  public constructor(i: int) Item := i; end constructor; "
    "end NoCmp; "
    "record Cmp: IComparable<Cmp> "
    "  public Item: int; "
    "  public constructor(i: int) Item := i; end constructor; "
    "  public method CompareTo(other: Cmp): int return Item - other.Item; "
    "    end CompareTo; "
    "end Cmp; "
    "class Ord: IComparable<Ord> "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  public method CompareTo(other: Ord): int return V - other.V; "
    "    end CompareTo; "
    "end Ord; "
    "class ClassNoCmp "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end ClassNoCmp; "
    "record AutoHashRec "
    "  public property X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "end AutoHashRec; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// A borrowed-parameter program: `body` runs inside a static method that takes
// the receiver `paramDecl` as a PARAMETER (never constructed in-unit) — the
// exact reachability shape #3490/#3865 describe: sema never checks the
// builtin generic's constraint for a non-`new` occurrence of the type.
std::string borrowedParamProg(const std::string& paramDecl,
                              const std::string& body) {
    return std::string(PRELUDE) +
           "class C "
           "  public static method Run(" + paramDecl + "): int " + body +
           "    return 0; "
           "  end Run; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// The constraint diagnostic fix #4318 (v0.315) now emits for these shapes,
// once, from SEMANTIC ANALYSIS (`argSatisfiesTypeBound`, spec §14.2 rule 2).
// That check fires BEFORE any lowering, so for every NAMED element/key type
// these rows exercise the sema message is what reaches the user and the
// codegen-time needles these rows originally pinned are no longer reachable
// for them — they survive only for shapes sema cannot decide on its own
// (`Object`, a bare type parameter; see sorted_set_class_element_3428_test.cpp,
// which still pins the codegen needles on exactly those).
//
// The needle is built per offending type name rather than shared, so a row
// can never pass on some OTHER type argument's constraint failure, and the
// tail deliberately stops BEFORE the constraint text: #4318's message quotes
// the stdlib constraint as `where T: IComparable`, the bare interface name,
// where stdlib §4.17/§4.18 and spec §14.2 rule 2 both spell it
// `where T: IComparable<T>`. That wording is issue #5423, and pinning only
// the spec's own part keeps every row here correct across its fix.
std::string constraintNeedle(const std::string& typeName) {
    return "type argument `" + typeName +
           "` does not satisfy the constraint `where ";
}

const char* NEEDLE_DICT_NOT_YET_IMPL =
    "is not yet supported in codegen";

}  // namespace

// ── #3865's OWN FILED REPRO, VERBATIM IN SHAPE ───────────────────────────────

// The exact program #3865 filed (a record holding a CLASS field — `Item: R`,
// `R` with a destructor — so `NoCmp` is doubly disqualified: neither
// auto-hashable (§9.2, an explicit field) nor `IComparable<NoCmp>`). Before
// this fix, `bezelc -c` aborted during LLVM IR verification with NO sema
// diagnostic at all; now it is a clean, located compile error.
BEZEL_TEST(sorted_set_no_compareto_3865, verbatim_issue_repro_rejected_not_crashed) {
    const char* src =
        "module M; import System; "
        "class R public V: int := 0; destructor end destructor; end R; "
        "record NoCmp "
        "  public Item: R; "
        "  public constructor(i: R) Item := i; end constructor; "
        "end NoCmp; "
        "class C "
        "  public static method CheckSorted(s: SortedSet<NoCmp>): bool "
        "    return s.Contains(s.First()); "
        "  end CheckSorted; "
        "  public static method Main(): int return 0; end Main; "
        "end C;";
    BEZEL_EXPECT_TRUE(rejectsWith(src, constraintNeedle("NoCmp")));
}

// ── SortedSet<T>: every member the §4.18 surface declares, on a BORROWED ────
// ── (never-constructed) receiver — the reachability shape #3490 describes ──

BEZEL_TEST(sorted_set_no_compareto_3865, contains_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<NoCmp>",
                          "var x := s.Contains(s.First()); "),
        constraintNeedle("NoCmp")));
}

// The CALL-syntax `.First()` (routes through `tryLowerCollectionInstanceCall`).
BEZEL_TEST(sorted_set_no_compareto_3865, first_call_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<NoCmp>", "var x := s.First(); "),
        constraintNeedle("NoCmp")));
}

// The PROPERTY-syntax `.First` (no parens — a bare `MemberAccessExpr`, the
// LlvmBackend_Exprs.cpp twin of the call-syntax arm above). Before this fix
// this compiled SILENTLY (no diagnostic at all, #2434 zero-degrade) rather
// than crashing — a MILDER but still real symptom of the identical gap.
BEZEL_TEST(sorted_set_no_compareto_3865, first_property_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<NoCmp>", "var x := s.First; "),
        constraintNeedle("NoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865, last_property_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<NoCmp>", "var x := s.Last; "),
        constraintNeedle("NoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865, add_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<NoCmp>",
                          "var it := new NoCmp(5); s.Add(it); "),
        constraintNeedle("NoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865, tryadd_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<NoCmp>",
                          "var it := new NoCmp(5); var ok := s.TryAdd(it); "),
        constraintNeedle("NoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865, remove_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<NoCmp>",
                          "var it := new NoCmp(5); var ok := s.Remove(it); "),
        constraintNeedle("NoCmp")));
}

// ── REVIEW FIX #3956: `genericBackingType`-ELIGIBLE elements with NO ────────
// ── `CompareTo` at all — a class, and an AUTO-HASHABLE record (issue ───────
// ── 3947's own shape) — in BOTH the CALL and PROPERTY spellings ────────────
//
// `NoCmp` above is genericBackingType-INELIGIBLE (neither auto-hashable nor
// CompareTo-dispatchable), so it exercises only the `!elemTy` early guard.
// `ClassNoCmp` and `AutoHashRec` are ELIGIBLE (`hasObjectHeaderEquality` /
// `recordTypeOfRef` respectively) with no `CompareTo`, so they exercise the
// SEPARATE `!cmpFn` guard fix #3956 added to the CALL-syntax arms — the
// PROPERTY-syntax arms (LlvmBackend_Exprs.cpp) already had it before #3956.

BEZEL_TEST(sorted_set_no_compareto_3865, class_no_compareto_call_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<ClassNoCmp>", "var x := s.First(); "),
        constraintNeedle("ClassNoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865, class_no_compareto_property_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<ClassNoCmp>", "var x := s.First; "),
        constraintNeedle("ClassNoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865,
          autohashable_record_no_compareto_call_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<AutoHashRec>", "var x := s.First(); "),
        constraintNeedle("AutoHashRec")));
}

BEZEL_TEST(sorted_set_no_compareto_3865,
          autohashable_record_no_compareto_property_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<AutoHashRec>", "var x := s.First; "),
        constraintNeedle("AutoHashRec")));
}

// ── FALSE-REJECT: the eligibility branches `ClassNoCmp`/`AutoHashRec` take ──
// ── (`hasObjectHeaderEquality` / `recordTypeOfRef`) are SHARED with ────────
// ── `Set<T>`/`Dictionary<K,V>` — confirm the mirrored guard did not leak ────
// ── into their (unrelated) code paths, with actual COMPUTED VALUES. ────────

BEZEL_TEST(sorted_set_no_compareto_3865, set_class_no_compareto_unaffected) {
    // `Set<T>`'s class-element path is the ARRAY-backed one (#1070) — the
    // real false-reject proof is that BOTH elements land (`Count == 2`) and
    // membership works (`.Contains`); #3956 must not have turned `.Add`/
    // `.Contains` into a silent no-op or a rejection for this shape.
    BEZEL_EXPECT_EQ(run(prog(
        "  using var a := new ClassNoCmp(3); using var b := new ClassNoCmp(1); "
        "  var s := new Set<ClassNoCmp>(); "
        "  s.Add(a); s.Add(b); "
        "  var n: int := s.Count; "
        "  if s.Contains(a) then n := n + 10; end; "
        "  return n; ")),
        12);
}

BEZEL_TEST(sorted_set_no_compareto_3865, set_autohashable_record_no_compareto_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new Set<AutoHashRec>(); "
        "  s.Add(new AutoHashRec(3)); s.Add(new AutoHashRec(1)); "
        "  s.Add(new AutoHashRec(3)); "          // structural-equality dup: no-op
        "  Console.WriteLine(s.Count); "          // 2 — dedup by Equals/GetHashCode
        "  Console.WriteLine((if s.Contains(new AutoHashRec(1)) then 1 else 0)); "
        "  return 0; ")),
        std::string("2\n1"));
}

// ── SortedDictionary<K, V>: the review-fix #3956 shapes, same two syntaxes ──

BEZEL_TEST(sorted_set_no_compareto_3865,
          sorteddictionary_class_key_no_compareto_call_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<ClassNoCmp, int>",
                          "var k := d.FirstKey(); "),
        constraintNeedle("ClassNoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865,
          sorteddictionary_class_key_no_compareto_property_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<ClassNoCmp, int>",
                          "var k := d.FirstKey; "),
        constraintNeedle("ClassNoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865,
          sorteddictionary_autohashable_record_key_no_compareto_call_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<AutoHashRec, int>",
                          "var k := d.FirstKey(); "),
        constraintNeedle("AutoHashRec")));
}

BEZEL_TEST(sorted_set_no_compareto_3865,
          sorteddictionary_autohashable_record_key_no_compareto_property_syntax_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<AutoHashRec, int>",
                          "var k := d.FirstKey; "),
        constraintNeedle("AutoHashRec")));
}

// ── FALSE-REJECT: `Dictionary<K,V>` shares the SAME `hasObjectHeaderEquality`/
// ── `recordTypeOfRef` key eligibility — confirm actual computed VALUES. ────

BEZEL_TEST(sorted_set_no_compareto_3865, dictionary_class_key_no_compareto_unaffected) {
    BEZEL_EXPECT_EQ(run(prog(
        "  var d := new Dictionary<ClassNoCmp, int>(); "
        "  d.Add(new ClassNoCmp(3), 30); d.Add(new ClassNoCmp(1), 10); "
        "  return d.Count; ")),
        2);
}

BEZEL_TEST(sorted_set_no_compareto_3865,
          dictionary_autohashable_record_key_no_compareto_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var d := new Dictionary<AutoHashRec, int>(); "
        "  d.Add(new AutoHashRec(3), 30); d.Add(new AutoHashRec(1), 10); "
        "  Console.WriteLine(d.Count); "
        "  Console.WriteLine(d[new AutoHashRec(1)]); "   // dedup/lookup by Equals/GetHashCode
        "  return 0; ")),
        std::string("2\n10"));
}

// ── SortedDictionary<K, V>: the KEY analogue, found by this issue's sweep ───

BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_add_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<NoCmp, int>",
                          "var k := new NoCmp(5); d.Add(k, 1); "),
        constraintNeedle("NoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_containskey_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<NoCmp, int>",
                          "var k := new NoCmp(5); var ok := d.ContainsKey(k); "),
        constraintNeedle("NoCmp")));
}

// PROPERTY-syntax `.FirstKey` (LlvmBackend_Exprs.cpp) — silently zero-degraded
// before this fix, with no diagnostic at all.
BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_firstkey_property_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<NoCmp, int>",
                          "var k := d.FirstKey; "),
        constraintNeedle("NoCmp")));
}

BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_lastkey_property_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<NoCmp, int>",
                          "var k := d.LastKey; "),
        constraintNeedle("NoCmp")));
}

// ── `Cmp` (declares a dispatchable `CompareTo`): a LEGAL §4.17/§4.18 key/ ───
// ── element — must NOT be told it "does not implement IComparable" ─────────

// `SortedSet<Cmp>` is #3740's own admitted shape: full functional coverage
// (not just "compiles") on the CONSTRUCTED, not merely borrowed, receiver —
// proving this fix's new `SortedSet` guard does not regress it.
BEZEL_TEST(sorted_set_no_compareto_3865, sortedset_cmp_element_full_functional) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<Cmp>(); "
        "  s.Add(new Cmp(30)); s.Add(new Cmp(10)); s.Add(new Cmp(20)); "
        "  s.Add(new Cmp(10)); "               // duplicate: CompareTo-equal, no-op
        "  var hasTen := (if s.Contains(new Cmp(10)) then 1 else 0); "
        "  var f := s.First().Item; "
        "  var l := s.Last().Item; "
        "  var removed := (if s.Remove(new Cmp(20)) then 1 else 0); "
        "  Console.WriteLine(s.Count); "        // 2 (30, 10 remain)
        "  Console.WriteLine(hasTen); "         // 1
        "  Console.WriteLine(f); "              // 10 (least, before Remove)
        "  Console.WriteLine(l); "              // 30 (greatest)
        "  Console.WriteLine(removed); "        // 1
        "  return 0; ")),
        std::string("2\n1\n10\n30\n1"));
}

// `SortedDictionary<Cmp, int>.FirstKey` (property syntax) IS already
// correctly implemented (`buildSortedKeyComparator`'s record-`CompareTo`
// branch, fix #3740) — this fix's new `!cmpFn` guard in
// LlvmBackend_Exprs.cpp must not reject it.
BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_cmp_key_firstkey_still_works) {
    // Pre-#3948 this test proved only that `.FirstKey` over a borrowed
    // `SortedDictionary<Cmp, int>` receiver COMPILES (`buildSortedKeyComparator`
    // already handled a record-`CompareTo` key from #3740) without the new
    // `!cmpFn` guard rejecting it; `Main`'s body (unconditional `return 0;`)
    // ran and determined the observed exit code. Post-#3948 the receiver is
    // actually USABLE end-to-end — see sorteddictionary_cmp_key_3948_test.cpp
    // for the full functional coverage — but the property-syntax `.FirstKey`
    // twin of the call-syntax diagnostic remains in force, so this row still
    // pins that guard does not reject the legal CompareTo-declaring shape.
    BEZEL_EXPECT_EQ(run(borrowedParamProg(
        "d: SortedDictionary<Cmp, int>",
        "var k := d.FirstKey; return k.Item; ")),
        0);
}

// `SortedDictionary<Cmp, int>.Add` is a LEGAL key per §4.17, and was the
// "not yet supported" diagnostic this fix (#3865) returned for a record key
// with a dispatchable `CompareTo` — a shape `SortedSet<T>`'s ELEMENT dispatch
// admitted under fix #3740 but whose KEY analogue had not been implemented.
// Fix #3948 closed that gap: `genericBackingType` now admits the record KEY
// the same way it admitted the ELEMENT, and the comparator build at
// construction (`buildSortedObjectKeyIdentity`, called for
// `colName == "SortedDictionary"` since fix #3488) and at `FirstKey`/
// `LastKey` (`buildSortedKeyComparator`'s own record-`CompareTo` arm from
// #3740) cover the runtime side. The dedicated regression file for that fix
// (sorteddictionary_cmp_key_3948_test.cpp) exercises this end-to-end; here
// we keep the false-reject guard below — `SortedDictionary<NoCmp, int>` (no
// `CompareTo`) MUST still get the "does not implement IComparable<K>"
// diagnostic, never the false positive that #3948's widening might
// accidentally produce if `recordHasDispatchableCompareTo` were ever called
// with the wrong type or the eligibility gate were dropped wholesale.
BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_nocmp_key_still_rejected) {
    const std::string src = borrowedParamProg(
        "d: SortedDictionary<NoCmp, int>",
        "var k := new NoCmp(5); d.Add(k, 1); ");
    BEZEL_EXPECT_TRUE(rejectsWith(src, constraintNeedle("NoCmp")));
    BEZEL_EXPECT_TRUE(lacks(src, NEEDLE_DICT_NOT_YET_IMPL));
}

// ── FALSE-REJECT GUARDS: this fix must not widen the rejection ──────────────

// `Set<Ord>`/`OwningSet<Ord>` (a class element with a dispatchable
// `CompareTo`, NOT SortedSet) keep their array-backed path untouched — the
// exact two rows that regressed during this fix's own development (a missing
// `headName() == "SortedSet"` guard on the new diagnostic briefly rejected
// these too; see sorted_set_class_element_3428_test.cpp for the full
// coverage this file does not duplicate).
BEZEL_TEST(sorted_set_no_compareto_3865, set_class_element_unaffected) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var o1 := new Ord(3); using var o2 := new Ord(1); "
        "  var s := new Set<Ord>(); "
        "  s.Add(o1); s.Add(o2); "
        "  var n: int := s.Count; "
        "  if s.Contains(o1) then n := n + 10; end; "
        "  return n; ")),
        12);
}

BEZEL_TEST(sorted_set_no_compareto_3865, owningset_class_element_unaffected) {
    BEZEL_EXPECT_EQ(run(prog(
        "  using var s := new OwningSet<Ord>(); "
        "  s.Add(new Ord(3)); s.Add(new Ord(1)); "
        "  return s.Count; ")),
        2);
}

// `SortedDictionary<string, V>` / `<int, V>` keep their pre-existing,
// legitimate fast paths — `genericBackingType` returns null for `string`
// (its OWN early return, not #3865's gap) and non-null for `int`.
BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_string_key_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var d := new SortedDictionary<string, int>(); "
        "  d.Add(\"b\", 2); d.Add(\"a\", 1); "
        "  Console.WriteLine((if d.ContainsKey(\"a\") then 1 else 0)); "
        "  Console.WriteLine(d.FirstKey); "
        "  Console.WriteLine(d.Count); "
        "  return 0; ")),
        std::string("1\na\n2"));
}

BEZEL_TEST(sorted_set_no_compareto_3865, sorteddictionary_int_key_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var d := new SortedDictionary<int, int>(); "
        "  d.Add(30, 3); d.Add(10, 1); "
        "  Console.WriteLine((if d.ContainsKey(10) then 1 else 0)); "
        "  Console.WriteLine(d.FirstKey); "
        "  Console.WriteLine(d.Count); "
        "  return 0; ")),
        std::string("1\n10\n2"));
}

// ── Count and iteration over a SortedSet are NOT over-rejected ─────────────
//
// #3865's own scope note here recorded that `.Count` and `for`-in over a
// `SortedSet<NoCmp>` stayed COMPILABLE — neither member needs a comparator
// (`Count` is `bz_dict_count`; the sorted-snapshot path declines to an
// UNSORTED snapshot rather than crashing), and the receiver was dead code
// anyway since `Add`/`TryAdd` were rejected.
//
// That scope boundary is now GONE, and correctly so: fix #4318's sema check
// (v0.315) rejects the TYPE ARGUMENT at the parameter declaration, so
// `SortedSet<NoCmp>` no longer resolves at all and no lowering — dormant or
// otherwise — can run on it. Spec §14.2 rule 2 requires the record to LIST
// `IComparable` and declare the members; `NoCmp` does neither, so the
// rejection is the spec's, not an over-reach.
//
// This row is therefore REPOINTED, not deleted: it now pins that the
// constraint check fires on the borrowed-PARAMETER spelling (the #3490
// reachability shape these rows exist for) and names the offending type.
// The companion row below restores the positive coverage the old row was
// really after — that `.Count`/iteration are not themselves over-rejected —
// over a CONFORMING element, where that question is still answerable.
BEZEL_TEST(sorted_set_no_compareto_3865, count_and_iteration_stay_unrejected_dormant) {
    BEZEL_EXPECT_TRUE(rejectsWith(borrowedParamProg(
        "s: SortedSet<NoCmp>",
        "var n := s.Count; "
        "for var e in s do var v := e.Item; end; "
        "return n; "),
        constraintNeedle("NoCmp")));
}

// The positive form of the question the row above used to ask: `.Count` and
// `for`-in over a SortedSet whose element CONFORMS are untouched by the
// constraint check and by #3865's new `SortedSet` member guard. Built and
// run for real (not via a never-invoked borrowed parameter), so the row
// asserts the values rather than merely the absence of a diagnostic.
BEZEL_TEST(sorted_set_no_compareto_3865, count_and_iteration_work_for_conforming_element) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<Cmp>(); "
        "  s.Add(new Cmp(30)); s.Add(new Cmp(10)); s.Add(new Cmp(20)); "
        "  Console.WriteLine(s.Count); "
        "  var acc := 0; "
        "  for var e in s do acc := acc * 100 + e.Item; end; "
        "  Console.WriteLine(acc); "
        "  return 0; ")),
        std::string("3\n102030"));
}
