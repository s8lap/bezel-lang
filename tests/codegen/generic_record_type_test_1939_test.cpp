// Codegen tests for §12.7/§12.9/§2.8.3 rule 4/§17.4 (#1939 leg 2, spec v0.220): runtime
// is/as/case type tests on BOXED GENERIC RECORDS honor the type arguments.
//
// §12.7: "`is T` tests whether a value is an instance of `T`"; §12.9 makes
// generic instantiations invariant, so a boxed `Pair<int, string>` is NOT an
// instance of `Pair<string, int>`. Pre-fix, a record box carried the SHARED
// per-declaration box vtable (setupBoxDescriptors keys boxVtable_/descArray_ by
// TypeSymbol), so every instantiation shared one runtime descriptor and
// `o is Pair<string, int>` FALSE-POSITIVE-matched a boxed `Pair<int, string>`
// (verified live on dd33abd: probe /var/tmp/bezel-scratch/laneE/
// p1939_record.bzl exits 11, expected 10) — the same erasure class #1843 fixed
// for classes and #2001 for inline-array sizes; the follow-on `as` unboxes the
// wrong layout (memory unsafety for layout-differing instantiations).
//
// The fix mints a per-monomorphization BOX vtable for a constructed generic
// record with concrete args: slot 0 becomes a chain of [the per-inst descriptor
// chain elements (instanceDescriptorFor — self + substituted interface
// instantiations), then the SHARED chain elements] — so precise targets
// (`is Pair<int,string>`, `is IM<int>`) match exactly, while the name-resolved
// destructuring pattern (`case o of Pair(a, b)`, which resolves the SHARED
// descriptor) keeps matching. perInstDescriptorForTarget learns Record targets,
// and boxRecordEqualsThunk compares the OTHER box's descriptor against the
// SELF box's own loaded descriptor (same instantiation ⇔ same chain global)
// instead of the static shared one.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// (1) The headline: `is` distinguishes sibling record instantiations.
// r1 = (o is Pair<int,string>) — genuine instance, must be 1;
// r2 = (o is Pair<string,int>) — sibling, must be 0. Expect 10.
// FAILS WITHOUT the fix (returns 11 — the erased false positive).
BEZEL_TEST(generic_record_type_test_1939, is_sibling_instantiation_false_12_9) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p: Pair<int, string> := new Pair<int, string>(1, \"x\"); "
        "  var o: Object := p; "
        "  var r1 := (if o is Pair<int, string> then 1 else 0); "
        "  var r2 := (if o is Pair<string, int> then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (2) `as` on the sibling instantiation is null; on the genuine one it unboxes
// the value (§17.4 — the result is nullable; §8.6 unboxing). Expect
// 1*100 (sibling null) + 0*10 (genuine non-null) + first-field 1 → 101.
BEZEL_TEST(generic_record_type_test_1939, as_sibling_null_genuine_unboxes_17_4) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p: Pair<int, string> := new Pair<int, string>(1, \"x\"); "
        "  var o: Object := p; "
        "  var bad := o as Pair<string, int>; "
        "  var good := o as Pair<int, string>; "
        "  var r1 := (if bad = null then 1 else 0); "
        "  var r2 := (if good = null then 1 else 0); "
        "  var f := 0; "
        "  if good <> null then f := good.first; end; "
        "  return r1 * 100 + r2 * 10 + f; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 101);
}

// (3) case typed-binding (§19.3) on a plain Object scrutinee: the sibling arm
// must NOT bind; the genuine arm must. Expect 2 (arm order pins the fix —
// pre-fix the FIRST arm erased-matched and returned 1; probe p1939_bind.bzl).
BEZEL_TEST(generic_record_type_test_1939, case_binding_selects_genuine_arm_19_3) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p: Pair<int, string> := new Pair<int, string>(1, \"x\"); "
        "  var o: Object := p; "
        "  case o of "
        "    var x: Pair<string, int> => do return 1; end, "
        "    var y: Pair<int, string> => do return 2; end, "
        "    _ => do return 0; end "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 2);
}

// (4) §19.3 — a bare generic record-destructuring pattern against an ERASED
// scrutinee is REJECTED.
//
// REWRITTEN 2026-07-29. This cell was a CONTROL asserting the shape still
// MATCHES (expect 1). `3f56e98` ("fix #2178: reject generic-record
// destructuring against an erased scrutinee") deliberately made it a compile
// error and did not update this pre-existing control, so it has been red on
// origin/main ever since — plain AND under ASan, and it blocked every gate
// mode until the flake-arbitration cap was fixed (c590173) and stopped hiding
// it.
//
// The rejection is the SOUND outcome, not a regression. `o` is typed `Object`,
// so the record's instantiation is not statically known: a box of ANY
// instantiation would match `Pair(a, b)`, and the arm's bindings would then
// read fields at the record's TEMPLATE layout — the wrong-typed-read hazard
// #2001/#2006 closed. The diagnostic names the remedy the spec prescribes: use
// a typed binding so the instantiation is statically determined, which is
// exactly what `case_binding_selects_genuine_arm_19_3` (cell 3 above) already
// covers with `var y: Pair<int, string> => ...`.
//
// So the positive path keeps its coverage in cell 3, and this cell now pins
// the rejection — including the diagnostic text, so a crash or an unrelated
// compile error cannot satisfy it (the harness maps both to the same -999
// sentinel that a reject produces).
BEZEL_TEST(generic_record_type_test_1939, destructure_by_name_erased_scrutinee_rejected_19_3) {
    auto r = runAot(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p: Pair<int, string> := new Pair<int, string>(1, \"x\"); "
        "  var o: Object := p; "
        "  case o of "
        "    Pair(a, b) => do return 1; end, "
        "    _ => do return 0; end "
        "  end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("names no type arguments") != std::string::npos) named = true;
    }
    BEZEL_EXPECT_TRUE(named);
}

// (5) CONTROL — non-generic records are untouched: `is` on the right and wrong
// record names behaves as before. Expect 10.
BEZEL_TEST(generic_record_type_test_1939, nongeneric_record_control_12_7) {
    int rc = run(
        "module M; "
        "record Rect(w: int, h: int); "
        "record Disc(r: int); "
        "class C public static method Main(): int "
        "  var v: Rect := new Rect(2, 3); "
        "  var o: Object := v; "
        "  var r1 := (if o is Rect then 1 else 0); "
        "  var r2 := (if o is Disc then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (6) NESTED argument: the descriptor key recurses (appendDescKey), so a
// nested-generic argument distinguishes too. Expect 10.
BEZEL_TEST(generic_record_type_test_1939, nested_generic_arg_distinguished_14_2) {
    int rc = run(
        "module M; "
        "record Box<T>(v: T); "
        "class C public static method Main(): int "
        "  var b: Box<Box<int>> := new Box<Box<int>>(new Box<int>(7)); "
        "  var o: Object := b; "
        "  var r1 := (if o is Box<Box<int>> then 1 else 0); "
        "  var r2 := (if o is Box<Box<string>> then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (7) A generic record IMPLEMENTING a generic interface: the specialized box
// chain carries the SUBSTITUTED interface instantiation, so `is IM<int>` is
// true and `is IM<string>` false on a boxed RBox<int> (§12.9 invariance
// through the implemented-interface projection). Expect 10.
BEZEL_TEST(generic_record_type_test_1939, implemented_iface_instantiation_12_9) {
    int rc = run(
        "module M; "
        "interface IM<T> end IM; "
        "record RBox<T>(v: T) : IM<T>; "
        "class C public static method Main(): int "
        "  var b: RBox<int> := new RBox<int>(7); "
        "  var o: Object := b; "
        "  var r1 := (if o is IM<int> then 1 else 0); "
        "  var r2 := (if o is IM<string> then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}

// (8) Boxed structural EQUALITY (§9.1/§9.6 via Object receivers): two boxes of
// the SAME instantiation compare structurally; a box of the SIBLING
// instantiation is a different runtime type → false (pre-fix the sibling pair
// erased-matched the descriptor gate and compared bytes of MISMATCHED layouts).
// Expect 10.
BEZEL_TEST(generic_record_type_test_1939, boxed_equals_same_vs_sibling_9_1) {
    int rc = run(
        "module M; "
        "record Pair<A, B>(first: A, second: B); "
        "class C public static method Main(): int "
        "  var p1: Pair<int, int> := new Pair<int, int>(1, 2); "
        "  var p2: Pair<int, int> := new Pair<int, int>(1, 2); "
        "  var q: Pair<string, int> := new Pair<string, int>(\"a\", 2); "
        "  var o1: Object := p1; "
        "  var o2: Object := p2; "
        "  var o3: Object := q; "
        "  var r1 := (if o1.Equals(o2) then 1 else 0); "
        "  var r2 := (if o1.Equals(o3) then 1 else 0); "
        "  return r1 * 10 + r2; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(rc, 10);
}
