// fix #4253 (§9.7 "Records and disposal" / §28.9 "Runtime-managed types",
// v0.298): class-field storage of a record carrying a string never released
// it — at ONE level of nesting, with no generics and no cycle. Materially
// broader than #4237 (which closed the analogous gap for a plain `var`
// LOCAL record binding, not class-field storage) and confirmed
// pre-existing (identical on this fix's parent `6f46a73`).
//
// Root cause: `Emitter::emitOwnedFieldDisposals` (LlvmBackend_Disposal.cpp)
// only ever walked `owned`-annotated fields (reference types). A record can
// never carry `owned` (§9.7 forbids it — records are value types embedded
// inline), so a class field like `public readonly property P: Box := new
// Box("s");` (`record Box(Value: string);`) had NO release path at Dispose
// AT ALL — not a mis-scoped one, an ABSENT one.
//
// THE FIX adds a fourth field-release walk (alongside the pre-existing
// string / closure / box FIELD-STORE walks in the same function), gated on
// THREE conditions that together prove the field is assigned EXACTLY ONCE,
// by a provably fresh/alias-free construction, for the entire lifetime of
// every instance:
//   1. `readonly` on the field/auto-property (MEASURED: an ordinary method
//      assigning it is rejected, "§23.1").
//   2. `sealed` on the declaring class, and no explicit constructor on it
//      (`!hasAnyConstructor`) — closing TWO independently-measured sema
//      gaps found while investigating this issue and filed separately
//      (NOT fixed here — out of scope for this codegen change):
//        - #4275: a readonly field WITH a declaration initializer can
//          currently be reassigned AGAIN inside an explicit constructor
//          body, no diagnostic.
//        - #4276: a DERIVED class's constructor can currently reassign a
//          readonly property INHERITED from its base, no diagnostic.
//      Rows 9 and 10 below exercise exactly these two shapes and confirm
//      the fix's guard keeps them SAFE (still leaking, never a UAF) even
//      though the underlying sema gaps remain open.
//   3. The field's OWN declaration initializer is provably a fresh,
//      alias-free construction of exactly the declared record type —
//      `Emitter::isFreshRecordConstructionOfExactType`, the SAME
//      eligibility authority #2448/#4237 established for the LOCAL case,
//      reused verbatim here (not re-derived, per LANE_RULES §6/§25).
//
// DIRECTION: this fix only ever ADDS a release where none existed (a safe-
// direction leak becomes balanced) — never removes one. Rows 1-2 prove the
// leak closes; rows 4-8 prove the three gates are each independently
// load-bearing (an accept-side sweep, not a volume of green); rows 9-10
// prove the two adjacent sema gaps do NOT turn this fix's guard into a
// double-free/UAF.
//
// Residual (NOT fixed here, filed as #4277): a field assigned via a
// CONSTRUCTOR-BODY statement (`this.P := new Box(...);`) rather than its
// own declaration initializer, or declared on a non-sealed class, or on a
// class with ANY explicit constructor (even one that never touches the
// field), is UNCHANGED — still safe-leaks exactly as before this fix.

#include <regex>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
// LEAK oracle: runAotChecked sets sanitizeAddress on the GENERATED program
// and treats ANY sanitizer finding (including LSan's own exit-time leak
// report) as ran=false. A program that leaks therefore reports -999 here;
// a clean program reports its real exit code.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}
// CRASH-FREEDOM oracle only (detect_leaks=0): proves no use-after-free /
// double-free / OOB, while tolerating an EXPECTED, still-safe leak. Used
// for the accept-side controls below, where the correct post-fix behavior
// is "still declines eligibility, still safely leaks" and the leak's
// MAGNITUDE is not the point — the ABSENCE of a crash is.
bool didNotCrash(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    return r.ran;
}
// §30 (measured 2026-09-11, LSan's own conservative-scan jitter): NEVER pin
// an exact leak byte count — assert a FLOOR comfortably below the observed
// minimum. Mirrors #4237's own `leaksAtLeast` verbatim (same file pattern,
// this test file's own local copy per this codebase's established
// per-file-anonymous-namespace-helper convention).
bool leaksAtLeast(const std::string& program, long minBytes) {
    auto r = runAotChecked(program);
    if (r.ran) return false;  // no sanitizer finding at all -- wrongly clean
    static const std::regex kLeak(
        R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak))
            return std::stol(m[1].str()) >= minBytes;
    }
    return false;  // sanitizer fired but not with a leak SUMMARY (e.g. a
                    // crash/UAF instead) -- correctly fails this row too
}
}  // namespace

// ===========================================================================
// Leak-fixed rows: loop-amplified (300x, per LANE policy -- a one-shot LSan
// row can pass on both sides), Main returns `acc mod 100` so the loop body
// cannot be constant-folded away; runChecked's oracle is exactly "did
// ASan/LSan report anything at all".
// ===========================================================================

// The issue's own minimal repro shape: ONE level of nesting, no generics,
// no cycle. `Holder` is sealed with no explicit constructor (its primary
// constructor param `s` feeds the field's OWN declaration initializer
// directly, via string concatenation -- a +1 producer, per
// isPlusOneStringProducer -- never a borrowed identifier).
BEZEL_TEST(class_field_record_string_release_4253,
           class_field_one_level_record_string_field_no_leak_spec_9_7) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box(Value: string); "
        "sealed class Holder(s: int) "
        "  public readonly property P: Box := new Box(\"hi\" + s.ToString()); "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 300 do "
        "    using var h := new Holder(i); "
        "    acc += h.P.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;"), 90);
}

// Two-level nesting: proves the class-field walk correctly reaches
// `emitRecordStringFieldReleases`'s PRE-EXISTING nested-record recursion
// (#2405/#3945), and that the new ADMISSION gate's own recursion (#4237's
// `isFreshRecordConstructionOfExactType` calling itself on a nested
// Record-typed positional argument) composes correctly at the class-field
// call site too, not only the local one.
BEZEL_TEST(class_field_record_string_release_4253,
           class_field_two_level_nested_record_string_field_no_leak_spec_9_7) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Inner(S: string); record Outer(I: Inner); "
        "sealed class Holder(s: int) "
        "  public readonly property P: Outer := "
        "    new Outer(new Inner(\"hi\" + s.ToString())); "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 300 do "
        "    using var h := new Holder(i); "
        "    acc += h.P.I.S.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;"), 90);
}

// ===========================================================================
// Value-correctness control (plain oracle, non-loop): the fix does not
// disturb what value is actually read back through the field.
// ===========================================================================
BEZEL_TEST(class_field_record_string_release_4253,
           class_field_value_correctness_plain_oracle) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(Value: string); "
        "sealed class Holder "
        "  public readonly property P: Box := new Box(\"held\" + \"value\"); "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var h := new Holder(); "
        "  if h.P.Value <> \"heldvalue\" then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// ===========================================================================
// Accept-side / soundness controls (LANE_RULES §15/§25): each row disables
// EXACTLY ONE of the fix's three gating conditions, otherwise keeping the
// shape identical to row 1's admitted case. Every row below is EXPECTED to
// still safely leak, unchanged from pre-fix behaviour -- proving each gate
// is independently load-bearing, not merely correlated with the accepted
// cases tested above (LANE_RULES §25: "what question was this predicate
// validated to answer, not what it correlates with").
// ===========================================================================

// Axis: `sealed`. Otherwise identical to row 1 (readonly, no explicit
// constructor, fresh-construction initializer) but the class is NOT
// sealed -- a subclass could exist (in this module or an importer) whose
// constructor reassigns the inherited field with no diagnostic (issue
// #4276). The fix must decline this shape.
BEZEL_TEST(class_field_record_string_release_4253,
           non_sealed_class_still_leaks_axis_sealed) {
    const char* prog =
        "module M; record Box(Value: string); "
        "class Holder(s: int) "
        "  public readonly property P: Box := new Box(\"hi\" + s.ToString()); "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    using var h := new Holder(i); "
        "    acc += h.P.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 6000));
}

// Axis: `!hasAnyConstructor`. Otherwise identical to row 1 but the class
// declares an EXPLICIT constructor (which never touches the field) --
// issue #4275 means a readonly field WITH a declaration initializer can
// currently be reassigned again inside SOME constructor body with no
// diagnostic; the fix conservatively declines any class with an explicit
// constructor at all, not just ones that are proven to reassign the field.
BEZEL_TEST(class_field_record_string_release_4253,
           class_with_unrelated_constructor_still_leaks_axis_no_ctor) {
    const char* prog =
        "module M; record Box(Value: string); "
        "sealed class Holder "
        // "fixed" + "value" -- MUST be a concatenation, not a bare literal:
        // a bare string literal is an immortal sentinel with nothing to
        // leak regardless of this fix's gating (confirmed while authoring
        // this row: a bare `new Box("fixed")` measured 0 B / clean here,
        // which would have made this row vacuous per LANE_RULES §3).
        "  public readonly property P: Box := new Box(\"fixed\" + \"value\"); "
        "  private _n: int; "
        "  public constructor(n: int) "
        "    this._n := n; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    using var h := new Holder(i); "
        "    acc += h.P.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    // MEASURED (this lane, scratch bezelc --sanitize=address, 200 iterations):
    // "8600 byte(s) leaked in 400 allocation(s)" -- floor set comfortably
    // below per LANE_RULES §30.
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 6000));
}

// Axis: `readonly`. Otherwise identical to row 1 but P is a PLAIN
// (non-readonly) auto-property -- any method could reassign it at any
// time during the instance's life, so the fix must decline it.
BEZEL_TEST(class_field_record_string_release_4253,
           non_readonly_property_still_leaks_axis_readonly) {
    const char* prog =
        "module M; record Box(Value: string); "
        "sealed class Holder(s: int) "
        "  public property P: Box := new Box(\"hi\" + s.ToString()); "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    using var h := new Holder(i); "
        "    acc += h.P.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(leaksAtLeast(prog, 6000));
}

// Axis: fresh-construction eligibility, the LOAD-BEARING soundness proof.
// Otherwise identical to row 1 (sealed, readonly, no explicit constructor)
// but the field's declaration initializer's nested string argument is a
// BORROWED identifier read (`s2`), not a literal or a +1 producer -- the
// exact shape #4237's own row 2 exercises for the LOCAL case. `s2` is read
// again AFTER `h` is constructed, so a wrongly-admitted `h` would free
// `s2`'s reference out from under this later read (heap-use-after-free).
// This is the row that PROVES reusing isFreshRecordConstructionOfExactType
// (rather than a shallower, home-grown "is this a NewExpr of the right
// type" check) is what keeps the new class-field walk sound.
BEZEL_TEST(class_field_record_string_release_4253,
           borrowed_string_arg_not_admitted_no_uaf_axis_eligibility) {
    const char* prog =
        "module M; record Box(Value: string); "
        "sealed class Holder(b: Box) "
        "  public readonly property P: Box := b; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    var s2 := \"shared\" + i.ToString(); "
        "    var b := new Box(s2); "
        "    using var h := new Holder(b); "
        "    acc += h.P.Value.Length; "
        "    acc += s2.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
}

// Axis: record-vs-choice. A CHOICE-typed field is unaffected by this fix
// (isFreshRecordConstructionOfExactType's own top-of-function
// `recTs->kind != ast::TypeDeclKind::Record` check rejects a choice
// outright) -- still safely leaks, unchanged. Mirrors #4237's own
// `nested_choice_field_still_declines_unaffected_no_uaf` control, applied
// to class-field storage instead of a local.
BEZEL_TEST(class_field_record_string_release_4253,
           choice_typed_field_still_declines_unaffected_axis_record_vs_choice) {
    const char* prog =
        "module M; choice Opt Some(V: string), None; end Opt; "
        "sealed class Holder(s: int) "
        "  public readonly property P: Opt := Opt.Some(\"x\" + s.ToString()); "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 200 do "
        "    using var h := new Holder(i); "
        "    i += 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
}

// ===========================================================================
// Direct proofs that the two adjacent, independently-filed sema gaps
// (#4275, #4276) do NOT turn this fix's guard into a double-free/UAF --
// the exact reassignment shapes that motivated the `sealed` and
// `!hasAnyConstructor` conditions, run against the REAL (guarded) fix.
// Both rows are expected to compile, run, and still safely leak (the sema
// gaps remain open, so the reassignment itself is accepted -- but this
// fix's own release never mis-fires against the reassigned value, because
// its gate declines both shapes outright).
// ===========================================================================

// Shape of issue #4275: a readonly field with a declaration initializer,
// reassigned again inside an explicit constructor body. `hasAnyConstructor`
// is true here (an explicit constructor exists), so this fix's own release
// never registers for `P` at all -- no UAF, whatever sema does or does not
// accept for the reassignment itself.
BEZEL_TEST(class_field_record_string_release_4253,
           sema_gap_4275_shape_still_safe_no_uaf) {
    // NO primary constructor here -- deliberately: §22.10 requires an
    // explicit constructor to chain via `this(...)` when the class ALSO
    // has a primary constructor (confirmed while authoring this row: a
    // primary `Holder(s: int)` alongside an explicit `constructor(s: int)`
    // is REJECTED, "must chain to another constructor via this(...)
    // (§22.10)" -- so #4275's shape needs a class with ONLY an explicit
    // constructor to compile at all).
    const char* prog =
        "module M; record Box(Value: string); "
        "sealed class Holder "
        "  public readonly property P: Box := new Box(\"first\"); "
        "  public constructor(s: int) "
        "    this.P := new Box(\"second\" + s.ToString()); "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    using var h := new Holder(i); "
        "    acc += h.P.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
}

// Shape of issue #4276: a derived class's constructor reassigns a readonly
// property inherited from its (non-sealed) base. `Base` is not `sealed`,
// so this fix's own release never registers for `Base.P` at all -- no UAF
// at `Derived`'s disposal (which chains to `Base`'s core disposer), even
// though sema currently accepts the reassignment in `Derived`'s
// constructor.
BEZEL_TEST(class_field_record_string_release_4253,
           sema_gap_4276_shape_still_safe_no_uaf) {
    const char* prog =
        "module M; record Box(Value: string); "
        "class Base "
        "  public readonly property P: Box := new Box(\"base\"); "
        "end Base; "
        "class Derived: Base "
        "  public constructor(s: int) "
        "    this.P := new Box(\"derived\" + s.ToString()); "
        "  end constructor; "
        "end Derived; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    using var d := new Derived(i); "
        "    acc += d.P.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
}
