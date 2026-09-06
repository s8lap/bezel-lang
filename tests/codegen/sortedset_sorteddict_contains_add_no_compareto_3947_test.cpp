// Spec-derived regression tests for fix #3947: `SortedSet<T>`/
// `SortedDictionary<K, V>` used to accept an auto-hashable-but-not-orderable
// element/key by one member family (`.Contains`/`.Add`/`.TryAdd`/`.Remove`/
// `.ContainsKey`/etc, silently falling back to STRUCTURAL §9.1/§9.2 equality)
// while REJECTING the identical receiver by another (`.First`/`.Last`/
// `.FirstKey`/`.LastKey`, fix #3865's own new diagnostic) — the same type,
// accepted by one member and rejected by another.
//
// AUTHORITY. stdlib §4.18 (v0.294), "A comparison-ordered set: membership by
// `CompareTo`-equality, iteration in ascending order", declares the SOLE
// constraint on `SortedSet<T>` as `where T: IComparable<T>`. stdlib §4.17
// states the identical shape for `SortedDictionary<K, V>`'s key
// (`where K: IComparable<K>`). Neither constraint mentions hashability — §3.6
// / §9.2 define hashable as a DIFFERENT, independent property (a record is
// hashable when it is auto-hashable OR declares user equality; a class,
// interface, and enum are always hashable via their §8.3 pair) — so an
// element/key can be hashable, orderable, both, or neither, and
// `genericBackingType` (LlvmBackend_Calls3.cpp)'s two "admit regardless of
// ordering" branches (the auto-hashable RECORD branch, `recordTypeOfRef`, and
// the class/interface branch, `hasObjectHeaderEquality`) exist ONLY because
// `Set<T>`/`Dictionary<K,V>` need hashing, not ordering — `SortedSet`/
// `SortedDictionary` merely INHERIT them with no additional check.
//
// THE DEFECT (measured on 5f1b77b, the fix #3865 HEAD this lane started
// from — verified on THIS tree before writing this file's fix, per the
// lane brief's own reproduction requirement).
//
//     module M;
//     record Pt
//       public property X: int;
//       public property Y: int;
//       public constructor(x: int, y: int) X := x; Y := y; end constructor;
//     end Pt;
//     class C
//       public static method Run(s: SortedSet<Pt>): int
//         var c := s.Contains(new Pt(1, 2));  // compiled clean, NO diagnostic
//         var f := s.First;                   // #3865's diagnostic (correct)
//         return 0;
//       end Run;
//       public static method Main(): int return 0; end Main;
//     end C;
//
// `s.Contains(...)` compiled with NO diagnostic at all, silently dispatching
// `bz_dict_contains_gen` through the STRUCTURAL (§9.1/§9.2) hash/eq
// trampolines `Pt`'s auto-hashability gives it — able to disagree with
// whatever `CompareTo`-based ordering the container is documented to
// maintain (there is none, since `Pt` declares no `CompareTo`) — while
// `s.First` on the SAME receiver, in the SAME function, correctly rejected.
// The identical inconsistency reproduces for `SortedDictionary<Pt, int>`
// (`.Add`/`.ContainsKey` compile silently; `.FirstKey` rejects).
//
// THE FIX (this file). `LlvmBackend_Calls.cpp`'s `tryLowerCollectionInstanceCall`
// now runs ONE check — "does this element/key, if it is only
// `genericBackingType`-eligible via the auto-hashable-record or
// class/interface branch, have a dispatchable `CompareTo`?" — BEFORE
// dispatching ANY member (`Add`/`TryAdd`/`Contains`/`Remove`/`Clear`/
// `Count()`/the set-algebra ops for `SortedSet`; `Add`/`TryAdd`/
// `ContainsKey`/`Remove`/`TryGetValue`/`GetOrNull`/`GetOrAdd` for
// `SortedDictionary`), reusing the SAME located diagnostic wording fix #3865
// already established for `.First`/`.Last`/`.FirstKey`/`.LastKey`. RESOLUTION
// (recorded per CLAUDE.md rule 4): reject the type at EVERY member rather
// than admit it everywhere with `CompareTo`-based semantics — the latter
// would require teaching the compiler an entirely new (unmeasured) ordered
// backing for an auto-hashable-with-`CompareTo` record, which fix #3740's
// own commit body already declined as a distinct, unmeasured change; the
// former reuses #3865's existing, already-reviewed infrastructure and is
// symmetric with how a class/interface with no `CompareTo` is ALREADY
// rejected at `new SortedSet<T>()` construction time (fix #2773 (B1) /
// narrow #3428) — this fix closes the identical gap for the reachability
// shape #3490 describes (a borrowed parameter, never locally constructed),
// where the construction-time check cannot fire.
//
// NARROW: an auto-hashable record that DOES declare `CompareTo` keeps its
// pre-existing STRUCTURAL identity unchanged (matches fix #3740's own
// restraint for the SortedSet-element analogue) — this fix is strictly about
// the "no comparator at all" shape, not a semantic widening.
//
// DOCUMENTED RESIDUAL (issue #3976, filed alongside this fix): the
// dictionary INDEXER (`sd[k]` / `sd[k] := v`, LlvmBackend_Assign.cpp) and
// `for`-in iteration (LlvmBackend_Stmts.cpp) consult `genericBackingType`
// through SEPARATE call sites this fix does not touch, so they still
// silently admit the identical no-`CompareTo` shape — pinned here as a KNOWN
// scope boundary (not a regression this file should mask), matching the
// project's own precedent for the "genuinely ineligible" shape
// (`sorted_set_sorted_dict_no_compareto_3865_test.cpp`'s
// `count_and_iteration_stay_unrejected_dormant`).
//
// A rejection is asserted on the diagnostic TEXT, never via a coarse
// `hasErrors()` or a bare "did not run": `runAot`'s WIFSIGNALED path
// synthesizes an `Error` diagnostic for a runtime crash too, so only the
// SPECIFIC needle text distinguishes a genuine, located rejection from a
// crash. Where a program compiles, its VALUE is asserted (Count, Contains
// results, indexer reads) — never "it compiled".

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

std::string out(const std::string& src) { return runAotOut(src); }

// A genuine compile-time rejection carrying `needle` in its message.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `Pt`: the ISSUE'S OWN shape — an auto-hashable RECORD (§9.2, two
// auto-PROPERTIES) with NO `CompareTo` at all. `AutoHashCmp`: the identical
// auto-hashable shape but WITH a dispatchable `CompareTo` — the NARROW
// false-reject control (this fix must not touch it). `ClsNoCmp`: a class
// with NO `CompareTo` — the class-side analogue of `Pt`, complementing
// `sorted_set_sorted_dict_no_compareto_3865_test.cpp`'s `ClassNoCmp`, whose
// coverage there is `.First`/`.First()` ONLY (this fix's own gap).
const char* PRELUDE =
    "module M; "
    "record Pt "
    "  public property X: int; "
    "  public property Y: int; "
    "  public constructor(x: int, y: int) X := x; Y := y; end constructor; "
    "end Pt; "
    "record AutoHashCmp "
    "  public property X: int; "
    "  public constructor(x: int) X := x; end constructor; "
    "  public method CompareTo(other: AutoHashCmp): int return X - other.X; "
    "    end CompareTo; "
    "end AutoHashCmp; "
    "class ClsNoCmp "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "end ClsNoCmp; ";

std::string prog(const std::string& body) {
    return std::string(PRELUDE) +
           "class C public static method Main(): int " + body +
           " end Main; end C;";
}

// A borrowed-parameter program: `body` runs inside a static method taking
// the receiver as a PARAMETER, never constructed in-unit — the exact
// reachability shape #3490/#3865/#3947 describe (sema never checks the
// builtin generic's constraint for a non-`new` occurrence of the type, so
// this is the ONLY way to reach a value of an otherwise-uninstantiable
// type — `Run`'s body is never actually invoked by `Main`).
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

const char* NEEDLE_SET =
    "requires the element type to implement `IComparable<T>`";
const char* NEEDLE_DICT =
    "requires the key type to implement `IComparable<K>`";

}  // namespace

// ── THE ISSUE'S OWN REPRO, VERBATIM IN SHAPE ────────────────────────────────

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           verbatim_issue_repro_contains_now_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<Pt>",
                          "var c := s.Contains(new Pt(1, 2)); "),
        NEEDLE_SET));
}

// The SAME receiver's `.First` (property syntax) already rejected before
// this fix (#3865) — pinned here to show BOTH members now agree, closing
// the inconsistency this issue is about.
BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           verbatim_issue_repro_first_still_rejected_consistently) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<Pt>", "var f := s.First; "),
        NEEDLE_SET));
}

// ── SortedSet<T>: AUTO-HASHABLE RECORD, NO CompareTo — every member ────────

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sortedset_autohashrecord_add_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<Pt>",
                          "var it := new Pt(1, 2); s.Add(it); "),
        NEEDLE_SET));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sortedset_autohashrecord_tryadd_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<Pt>",
                          "var it := new Pt(1, 2); var ok := s.TryAdd(it); "),
        NEEDLE_SET));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sortedset_autohashrecord_remove_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<Pt>",
                          "var it := new Pt(1, 2); var ok := s.Remove(it); "),
        NEEDLE_SET));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sortedset_autohashrecord_clear_rejected) {
    // Not an identity-reading member — pins that the fix rejects the type at
    // EVERY member reachable through this arm, not only the ones that read
    // `CompareTo`/equality, per this issue's own resolution.
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<Pt>", "s.Clear(); "), NEEDLE_SET));
}

// ── SortedSet<T>: CLASS, NO CompareTo — the class-side analogue ────────────
// ── (3865's own coverage for this shape is `.First`/`.First()` only) ───────

// The argument is a FRESH temporary (`new ClsNoCmp(5)`), not bound to a named
// local first — a class-typed `.Add` argument routed through a named local
// hits an UNRELATED §28.11 borrow-lifetime diagnostic before codegen ever
// runs (a borrowed parameter receiver outlives a plain local source), which
// would mask whether THIS fix's own diagnostic fires at all. A fresh
// temporary reaches codegen (alongside its own, unrelated §15.4 fresh-store
// diagnostic — MEASURED, both diagnostics are present in the same compile),
// where `rejectsWith` finds this fix's diagnostic among the set.
BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sortedset_class_no_compareto_add_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<ClsNoCmp>", "s.Add(new ClsNoCmp(5)); "),
        NEEDLE_SET));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sortedset_class_no_compareto_contains_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("s: SortedSet<ClsNoCmp>",
                          "var it := new ClsNoCmp(5); var c := s.Contains(it); "),
        NEEDLE_SET));
}

// ── SortedDictionary<K, V>: AUTO-HASHABLE RECORD key, NO CompareTo ─────────

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sorteddictionary_autohashrecord_key_add_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<Pt, int>",
                          "var k := new Pt(1, 2); d.Add(k, 1); "),
        NEEDLE_DICT));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sorteddictionary_autohashrecord_key_tryadd_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<Pt, int>",
                          "var k := new Pt(1, 2); var ok := d.TryAdd(k, 1); "),
        NEEDLE_DICT));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sorteddictionary_autohashrecord_key_containskey_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<Pt, int>",
                          "var k := new Pt(1, 2); var ok := d.ContainsKey(k); "),
        NEEDLE_DICT));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sorteddictionary_autohashrecord_key_remove_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<Pt, int>",
                          "var k := new Pt(1, 2); var ok := d.Remove(k); "),
        NEEDLE_DICT));
}

// ── SortedDictionary<K, V>: CLASS key, NO CompareTo ────────────────────────

// Fresh temporary key, same reason as `sortedset_class_no_compareto_add_rejected`
// above — a named-local key argument hits the unrelated §28.11 borrow-lifetime
// diagnostic before codegen runs.
BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sorteddictionary_class_key_no_compareto_add_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<ClsNoCmp, int>",
                          "d.Add(new ClsNoCmp(5), 1); "),
        NEEDLE_DICT));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sorteddictionary_class_key_no_compareto_containskey_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        borrowedParamProg("d: SortedDictionary<ClsNoCmp, int>",
                          "var k := new ClsNoCmp(5); var ok := d.ContainsKey(k); "),
        NEEDLE_DICT));
}

// ── NARROW false-reject: auto-hashable + CompareTo is left ALONE ───────────
// ── (fix #3740's own restraint) — VALUES, not just "compiles" ─────────────

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sortedset_autohashable_record_with_compareto_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var s := new SortedSet<AutoHashCmp>(); "
        "  s.Add(new AutoHashCmp(3)); s.Add(new AutoHashCmp(1)); "
        "  s.Add(new AutoHashCmp(3)); "  // structural-equality dup: no-op
        "  Console.WriteLine(s.Count); "  // 2 — dedup by Equals/GetHashCode
        "  Console.WriteLine((if s.Contains(new AutoHashCmp(1)) then 1 "
        "else 0)); "
        "  return 0; ")),
        std::string("2\n1"));
}

BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           sorteddictionary_autohashable_record_key_with_compareto_unaffected) {
    BEZEL_EXPECT_EQ(out(prog(
        "  var d := new SortedDictionary<AutoHashCmp, int>(); "
        "  d.Add(new AutoHashCmp(3), 30); d.Add(new AutoHashCmp(1), 10); "
        "  Console.WriteLine(d.Count); "
        "  Console.WriteLine(d[new AutoHashCmp(1)]); "  // lookup by Equals/GetHashCode
        "  return 0; ")),
        std::string("2\n10"));
}

// ── DOCUMENTED RESIDUAL SCOPE BOUNDARY (issue #3976) ───────────────────────
// ── The indexer and for-in loop are SEPARATE call sites this fix does ─────
// ── not touch — pinned as KNOWN, not silently regressed into a crash. ─────

// The indexer (`d[k] := v` / `d[k]`) still silently admits a no-`CompareTo`
// class key via structural (§8.3) identity — LlvmBackend_Assign.cpp's own
// `genericBackingType` consultation, untouched by this fix. UNLIKE the
// rejected-member rows above, `SortedDictionary<ClsNoCmp, int>` CAN still be
// constructed and populated (via the indexer, not `.Add`/`.TryAdd`, both
// rejected above) in a well-typed program, so this is a REAL, INVOKED
// runtime-VALUE check (via `prog()`/`Main`, not the never-invoked
// `borrowedParamProg` shape the rejection rows use) — not merely "compiles".
BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           residual_indexer_still_admits_no_compareto_key_issue_3976) {
    BEZEL_EXPECT_EQ(run(prog(
        "  var d := new SortedDictionary<ClsNoCmp, int>(); "
        "  var k := new ClsNoCmp(5); "
        "  d[k] := 1; "
        "  return d[k]; ")),
        1);
}

// `for`-in iteration over a `SortedSet<T>` whose element has no `CompareTo`
// at all stays reachable and compiling (LlvmBackend_Stmts.cpp) — this row's
// oracle is COMPILE-STABILITY, not a runtime value: `Pt` can no longer be
// legally INSERTED into a `SortedSet<Pt>` at all (every insertion member is
// rejected above), so no well-typed program can ever produce a non-empty
// receiver to iterate and observe a value from — `run()` here only proves
// `bezelc` still lowers this dead-but-declared method body without a
// malformed-IR crash (every declared method is codegen'd whether or not it
// is ever called), mirroring
// `sorted_set_sorted_dict_no_compareto_3865_test.cpp`'s own
// `count_and_iteration_stay_unrejected_dormant` precedent for the
// "genuinely ineligible" shape.
BEZEL_TEST(sortedset_sorteddict_contains_add_no_compareto_3947,
           residual_forin_iteration_stays_unrejected_dormant_issue_3976) {
    BEZEL_EXPECT_EQ(run(borrowedParamProg(
        "s: SortedSet<Pt>",
        "var n := 0; for var e in s do n := n + e.X; end; return n; ")),
        0);
}
