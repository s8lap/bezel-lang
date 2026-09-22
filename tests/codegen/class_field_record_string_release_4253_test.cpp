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
// a proof that the field is assigned EXACTLY ONCE, by a provably fresh /
// alias-free construction, for the entire lifetime of every instance.
//
// #4253 built that proof out of THREE structural proxies — `readonly` on
// the member, `sealed` on the declaring class, `!hasAnyConstructor(ts)` —
// and recorded the residual as issue #4277.
//
// fix #4277 (§9.7 / §28.9 / §13.2 / §23.1, v0.306) REPLACES all three with
// the ONE question they were each a coarse answer to: is the field's own
// declaration initializer the SOLE store its slot ever receives?
// `Emitter::declarationInitializerIsSoleStore` (LlvmBackend_Setup.cpp)
// answers it from two facts rather than three proxies —
//
//   (A) no write site can exist in a module this compilation cannot read.
//       §13.2 defaults a top-level type to `internal`, so an undecorated
//       class is unnameable outside its module; §23.1 (+ fix #4276's
//       declaring-type narrowing) confines a `readonly` member's writes to
//       its declaring type's own field initializer and constructors; and an
//       IMPORTED declaring type, whose bodies never cross the §2.8.2
//       interface, must therefore be `readonly` with no constructor at all
//       (the one place #4253's `hasAnyConstructor` proxy was real evidence
//       rather than a stand-in, and the one place it is kept).
//   (B) no write site exists in THIS module, per a module-wide scan of
//       every declaration body for an assignment / increment / `ref` /
//       `out` position resolving to the member. The scan is fail-closed:
//       an AST node kind it does not recognize disables every admission.
//
// Three shapes #4253 declined are admitted as a result, each measured
// leaking on origin/main cafdfc52 and clean with the fix (200-iteration
// loops): a NON-SEALED class (7490 B / 400 allocations), a class with an
// UNRELATED explicit constructor (8600 B / 400), and a NON-READONLY
// auto-property (7490 B / 400). Their three sanitizer_leak_exempt.txt rows
// are removed with this fix.
//
// Its OWN eligibility authority is unchanged: the field's declaration
// initializer must still be a provably fresh, alias-free construction of
// exactly the declared record type — `Emitter::isFreshRecordConstruction
// OfExactType`, the SAME proof #2448/#4237 established for a plain `var`
// record local, reused verbatim (LANE_RULES §6/§25).
//
// DIRECTION: this fix only ever ADDS a release where none existed (a safe-
// direction leak becomes balanced) — never removes one. Rows 1-2 prove the
// original leak closes; the three #4277 rows prove the three axes close;
// the DECLINE controls prove each half of the predicate is load-bearing
// (removing either turns a control into a use-after-free, not a leak);
// rows 9-10 keep the two sema-gap shapes (#4275 accepted-by-design, #4276
// now rejected) honest.
//
// STILL RESIDUAL under #4277, deliberately NOT closed here: a field
// assigned by a CONSTRUCTOR-BODY statement (`this.P := new Box(...);`)
// instead of its own declaration initializer. The predicate correctly
// declines it — a constructor-body store IS a write site — and closing it
// needs the store itself to transfer a +1 (drop-on-overwrite for record-
// typed fields, §28.7's reference-field mechanism generalized), which is a
// different change from proving how many stores there are.

#include <regex>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
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
// fix #4277 (§9.7 / §28.9 / §13.2 / §23.1, v0.306) — THE THREE AXES #4253
// LEFT OPEN, NOW CLOSED.
//
// Each row below disables EXACTLY ONE of #4253's three structural gates,
// otherwise keeping the shape identical to row 1's admitted case. Until
// #4277 each was EXPECTED to still safely leak, and each carried a row in
// scripts/gate/sanitizer_leak_exempt.txt positively asserting that leak:
//
//   class_field_record_string_release_4253.non_sealed_class_still_leaks_axis_sealed
//   class_field_record_string_release_4253.class_with_unrelated_constructor_still_leaks_axis_no_ctor
//   class_field_record_string_release_4253.non_readonly_property_still_leaks_axis_readonly
//
// All three exempt rows are REMOVED by this fix, and all three programs are
// now leak-free. MEASURED on origin/main cafdfc52 (bezelc --sanitize=address,
// the exact 200-iteration programs below): 7490 B / 400 allocations,
// 8600 B / 400 allocations, and 7490 B / 400 allocations respectively —
// clean on all three with the fix.
//
// The three gates were not three defects: they were three coarse answers to
// ONE question — "is the field's own declaration initializer the SOLE store
// this slot ever receives?" — which `declarationInitializerIsSoleStore`
// (LlvmBackend_Setup.cpp) now asks directly. The DECLINE controls that keep
// that predicate honest are the rows further down, not these three.
//
// Each row asserts the exact accumulated value (`acc mod 100`), not merely
// leak-freedom: `runChecked` fails on ANY sanitizer finding, so a release
// emitted at the wrong place would surface here as a use-after-free rather
// than as a silently-different number.
// ===========================================================================

// Axis: `sealed`. Otherwise identical to row 1 (readonly, no explicit
// constructor, fresh-construction initializer) but the class is NOT
// sealed. #4253 declined this because a subclass's constructor could
// reassign the inherited field with no diagnostic — a sema gap that WAS
// real when #4253 was written and was closed by fix #4276 (the four
// readonly-write gates now require `currentType_ == ps->parent`). With
// §23.1 confining a readonly write to the declaring type's own body and
// the module-wide write-site scan finding no such write, `sealed` is not
// evidence this predicate needs.
BEZEL_TEST(class_field_record_string_release_4253,
           non_sealed_class_record_string_field_no_leak_spec_9_7_issue_4277) {
    BEZEL_EXPECT_EQ(runChecked(
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
        "end Main; end C;"), 90);
}

// Axis: `!hasAnyConstructor`. Otherwise identical to row 1 but the class
// declares an EXPLICIT constructor that never touches the field. #4253
// declined every class with any constructor at all, because it had no way
// to tell "a constructor exists" from "a constructor writes this field".
// The write-site scan tells them apart: this constructor assigns `_n`, not
// `P`, so `P` still has exactly one store.
BEZEL_TEST(class_field_record_string_release_4253,
           class_with_unrelated_constructor_record_string_field_no_leak_spec_9_7_issue_4277) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; record Box(Value: string); "
        "sealed class Holder "
        // "fixed" + "value" -- MUST be a concatenation, not a bare literal:
        // a bare string literal is an immortal sentinel with nothing to
        // leak regardless of this fix's gating (confirmed while authoring
        // this row: a bare `new Box("fixed")` measured 0 B / clean here,
        // which would have made the pre-#4277 leak assertion vacuous per
        // LANE_RULES §3 — and would make THIS row prove nothing either).
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
        "end Main; end C;"), 0);
}

// Axis: `readonly`. Otherwise identical to row 1 but P is a PLAIN
// (non-readonly) auto-property, so §23.1 does not confine its writes at
// all — any method in this module, and an importer if it could name the
// type, may assign it. #4253 declined on that possibility alone. The
// predicate instead checks the two facts that decide it: §13.2 makes the
// undecorated `class Holder` **internal**, so no importer can name it, and
// the module-wide scan finds no assignment to `P` anywhere. (Remove EITHER
// fact and the shape is correctly declined again — see the two decline
// controls `non_readonly_property_reassigned_by_method_declined_no_uaf`
// and `public_type_public_non_readonly_property_declined` below.)
BEZEL_TEST(class_field_record_string_release_4253,
           non_readonly_property_record_string_field_no_leak_spec_9_7_issue_4277) {
    BEZEL_EXPECT_EQ(runChecked(
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
        "end Main; end C;"), 90);
}

// ===========================================================================
// DECLINE controls for the #4277 predicate (LANE_RULES §15/§25). These are
// the rows that make `declarationInitializerIsSoleStore` load-bearing: each
// removes ONE of the two facts its proof rests on, and each would become a
// use-after-free — not a leak — if the predicate admitted it anyway.
// ===========================================================================

// (B) half: the module-wide WRITE-SITE scan. Same non-readonly shape as the
// `readonly`-axis row above, except a method assigns `this.P := b` from a
// BORROWED parameter, so the slot ends up aliasing a record the caller still
// owns. `keep`'s string is read AFTER the Holder is disposed, so admitting
// this shape would free `keep.Value` out from under that read — a
// heap-use-after-free, which `runAotCheckedLenient` reports as ran=false.
// Deleting the scan from the predicate turns this row red.
BEZEL_TEST(class_field_record_string_release_4253,
           non_readonly_property_reassigned_by_method_declined_no_uaf) {
    const char* prog =
        "module M; record Box(Value: string); "
        "sealed class Holder "
        "  public property P: Box := new Box(\"fresh\" + \"init\"); "
        "  public method Take(b: Box) "
        "    this.P := b; "
        "  end Take; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var i := 0; "
        "  var acc := 0; "
        "  while i < 200 do "
        "    var keep := new Box(\"kept\" + i.ToString()); "
        "    do "
        "      using var h := new Holder(); "
        "      h.Take(keep); "
        "    end; "
        "    acc += keep.Value.Length; "
        "    i += 1; "
        "  end; "
        "  return acc mod 100; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// (A1) half: cross-module reachability. A `public` type with a `public`,
// NON-readonly property is the one shape whose write sites this compilation
// genuinely cannot enumerate — an importer may assign `h.P` with a value
// this module never sees (§13.2 makes the type nameable there; §23.1
// confines nothing, the member not being readonly). The predicate declines
// it, so it still safely leaks exactly as before #4277.
//
// HONEST LABEL: this row passes on BOTH sides of #4277 — the decline is
// what it pins, and a single-module program cannot exhibit the importing
// module's write. It is a recorded-decision control, not a FAIL-WITHOUT
// row. The `didNotCrash` oracle (detect_leaks=0) is used deliberately so
// the still-present leak needs no sanitizer_leak_exempt.txt declaration.
BEZEL_TEST(class_field_record_string_release_4253,
           public_type_public_non_readonly_property_declined) {
    const char* prog =
        "module M; public record Box(Value: string); "
        "public sealed class Holder(s: int) "
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
    BEZEL_EXPECT_TRUE(didNotCrash(prog));
    BEZEL_EXPECT_EQ(run(prog), 90);
}

// Axis: fresh-construction eligibility. Otherwise identical to row 1
// (sealed, readonly, no explicit constructor) but the field's OWN
// declaration initializer is `P := b` -- a bare read of the primary-ctor
// parameter `b` (itself `Box`-typed), not a `new Box(...)` call at all.
// CORRECTED (send-back on this fix's review): an earlier revision of this
// comment described this row as exercising #4237's own row 2 (a nested
// BORROWED string argument inside an ADMITTED `new Box(...)` call, declined
// by the nested-argument nested string-producer check) -- that is a
// DIFFERENT, deeper shape this row does not exercise. `P := b` declines at
// `isFreshRecordConstructionOfExactType`'s very FIRST check
// (`dynamic_cast<const ast::NewExpr*>(expr)` fails immediately, since `b`
// is an IdentifierExpr, not a NewExpr) -- the shallowest possible decline,
// before any nested-argument analysis is ever reached. It still proves a
// real thing: a plain aliasing property assignment is correctly declined by
// the coarse "is this even a NewExpr" gate, so a wrongly-admitted `h` would
// alias `b`'s (and hence `s2`'s) string -- `s2` is read again AFTER `h` is
// constructed, so a wrongly-admitted `h` would free `s2`'s reference out
// from under this later read (heap-use-after-free), still a real UAF-shaped
// oracle, just via the coarser mechanism. `new Box(b.Value)` -- a borrowed
// FIELD read as the constructor argument, the shape the ORIGINAL comment
// described -- was separately measured (this send-back, `agent_check.sh
// --probe --asan`, same 200-iteration shape): declines at the NESTED
// check instead (`b.Value` is neither a string literal nor a +1 producer),
// still safe-leaks cleanly -- "Direct leak of 6400 byte(s) in 200 object(s)"
// + "Indirect leak of 1890 byte(s) in 200 object(s)", SUMMARY 8290 B / 400
// allocations, no crash -- exercising the nested-argument check this row's
// own program does not reach.
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
// UPDATED post-issue-4276 (see that fix's own commit for the full
// analysis): row 9 still proves the #4275 shape (a DECLINED-to-fix,
// deliberately-accepted behaviour, DECLINED.md v0.271) does not turn this
// fix's guard into a double-free/UAF. Row 10 no longer proves the parallel
// claim for #4276 -- that shape is now a compile-time REJECTION, so there
// is no longer a reassigned value for this fix's release to mis-fire
// against; row 10 asserts the rejection directly instead.
// ===========================================================================

// Shape of issue #4275: a readonly field with a declaration initializer,
// reassigned again inside an explicit constructor body. §22.8's own scope
// statement and DECLINED.md (v0.271) record that the single-assignment rule
// deliberately does NOT extend to readonly auto-properties, so this program
// is ACCEPTED by design and the constructor-body store overrides the
// initializer at runtime (#3207).
//
// UPDATED for fix #4277 — same verdict, better reason. #4253 declined it
// because `hasAnyConstructor(ts)` was true; the #4277 predicate declines it
// because the module-wide write-site scan SEES the `this.P := new Box(...)`
// store, so `P`'s declaration initializer is demonstrably not its sole
// store. That distinction is what lets the unrelated-constructor row far
// above be admitted while this one stays declined: the two shapes differ
// only in whether the constructor writes THIS field, and only the scan can
// tell them apart. No UAF either way.
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

// Shape of issue #4276, UPDATED post-fix-#4276 (was
// `sema_gap_4276_shape_still_safe_no_uaf`, asserting `didNotCrash` on an
// ACCEPTED program): a derived class's constructor reassigning a readonly
// property inherited from its (non-sealed) base is now a compile-time
// REJECTION — issue 4276 added the missing `currentType_ ==
// ps->parent`/`fs->parent` declaring-type check to the four readonly-write
// gates in ExprTypecheck.cpp, closing exactly this shape (the explicit
// `this.P` spelling now agrees with the bare `P := e` spelling, which was
// already rejected). The program below therefore no longer compiles at
// all, so no release condition gets a chance to matter for THIS vector.
//
// UPDATED for fix #4277: #4253's condition 2 (`sealed` +
// `!hasAnyConstructor`) is GONE — fix #4276 is precisely what made the
// `sealed` half unnecessary, since a derived constructor can no longer
// reach an inherited readonly member from any module, and the write-site
// scan replaced the `!hasAnyConstructor` half for every module this
// compilation CAN read. This row keeps pinning the rejection itself, which
// is the fact (A)'s readonly argument now leans on.
BEZEL_TEST(class_field_record_string_release_4253,
           derived_ctor_inherited_readonly_reassignment_rejected_spec_23_1_issue_4276) {
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
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "cannot assign to readonly property `P`"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(r, "§23.1"));
}
