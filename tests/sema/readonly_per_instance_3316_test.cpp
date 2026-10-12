// Spec-derived sema tests for fix #3316 / ruling #3299 (v0.270):
// the `readonly` assignable context is per-INSTANCE, not per-type.
//
// Authority: §22.8 / §23.1 (v0.270) — "an instance readonly field is
// assignable only through an lvalue chain rooted at `this` — an explicit
// `this.F := e` or the bare in-scope spelling that resolves to `this`'s
// storage — so a constructor of the declaring type may NOT write another
// value's readonly storage held in a parameter, local, or field". The
// pre-fix compiler used a bare `if (inConstructor_) return;` (and its
// `:=` / ref/out / property siblings), so any constructor body could
// write through any other instance's readonly field. §7.4's narrowing
// licence assumes such a field has "no writer on any path, through any
// alias" outside its construction window — a per-type carve-out silently
// falsified the licence.
//
// The fix narrows the four construction-window carve-out sites in
// `ExprTypecheck.cpp` (`checkReadonlyAssignment` field branch, the
// positional-record sibling, `checkPropertyWriteTarget`, and the two
// `ref`/`out` field/property operand checks) to `inConstructor_ &&
// chainIsThisRooted`. The static readonly carve-out is preserved
// unchanged (per-instance vs per-type does not reach it; v0.270 records
// the case as open, the issue explicitly scopes it out).
//
// Tests:
//   1. REJECT: `other._f := 99` from C's constructor (cross-instance
//      readonly field write — the §22.8 per-instance ruling's headline
//      case)
//   2. REJECT: same, but spelled `other._f := 99` from a SECOND C
//      constructor (e.g. a convenience constructor that takes a C
//      parameter) — different receiver shape, same defect
//   3. REJECT: `ref other._f` from C's constructor (§22.3 inherits the
//      context; the §22.8 ruling's ref/out twin)
//   4. REJECT: `ref other.X` where X is a positional record's
//      auto-generated property, from R's own constructor (the
//      same-instance case the previous positional-record fix left open)
//   5. REJECT: `other._f := 99` from a pure ctor — pin the existing
//      §22.15 this-rooted birth-store rejection (already in place via
//      PureChecker.cpp's targetRootSymbol walk) so it cannot regress
//   6. ACCEPT (control): `this._f := 99` from C's constructor — the
//      §22.8 per-instance spelling, must keep compiling
//   7. ACCEPT (control): bare `_f := 99` from C's constructor — the
//      §22.8 bare-in-scope spelling, must keep compiling
//   8. ACCEPT (control): `C._s := 99` from C's ctor — static readonly
//      carve-out preserved (`TypeName._s` is a bare type-name root, NOT
//      this-rooted, but the static modifier leaves the carve-out alone)
//   9. ACCEPT (control): bare static `_s := 99` from C's ctor — the
//      implicit-this spelling for a static field still works (the
//      static modifier leaves the carve-out alone)

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/PureChecker.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// =========================================================================
// §22.8 / §23.1 (v0.270, #3299/#3316) — REJECTION SHAPES
// =========================================================================

// HEADLINE CASE: a constructor of C writing to ANOTHER C's `readonly`
// field through an instance receiver. Pre-fix: silently accepted by
// `if (inConstructor_) return;` and codegen wrote through `other`'s
// backing storage, falsifying §7.4's narrowing licence. Post-fix: the
// per-instance ruling rejects because the chain's base is `other`, not
// `this`.
BEZEL_TEST(readonly_per_instance_3316,
           ctor_cross_instance_readonly_field_assignment_rejected_spec_22_8_issue_3316) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _f: int; "
        "  public constructor(other: C) other._f := 99; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "cannot assign to readonly field `_f`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "declaring class's constructor"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.8"));
}

// SAME CASE through a SECOND convenience constructor that takes a C
// parameter — distinct AST shape (a constructor declaration with an
// `other: C` parameter), same defect. The cross-instance WRITE is on
// the LHS (`other._f := ...`); the RHS would be a separate concern (a
// cross-instance READ is allowed, since reads are not bound by the
// readonly modifier — they just see the value at construction's end).
BEZEL_TEST(readonly_per_instance_3316,
           second_ctor_cross_instance_readonly_field_assignment_rejected_spec_22_8_issue_3316) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _f: int; "
        "  public constructor() _f := 1; end constructor; "
        "  public constructor(other: C) other._f := 99; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    // The second ctor's body writes `other._f` — a cross-instance
    // readonly field write (the LHS of `:=`); the first ctor's
    // `_f := 1` is the bare-implicit-this spelling and is legitimate.
    // Exactly one diagnostic should fire (on the cross-instance write).
    int crossInstanceDiagCount = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("cannot assign to readonly field `_f`") !=
                std::string::npos &&
            d.message.find("declaring class's constructor") !=
                std::string::npos) {
            crossInstanceDiagCount++;
        }
    }
    BEZEL_EXPECT_TRUE(crossInstanceDiagCount == 1);
}

// §22.3 / §22.8 ref/out twin: `ref other._f` from C's constructor binds
// another value's readonly storage to a ref parameter — same store by
// other syntax. Pre-fix: `checkRefOutFieldOperand` had the same bare
// `!inConstructor_` guard as the `:=` sibling, so this slipped through.
BEZEL_TEST(readonly_per_instance_3316,
           ctor_ref_operand_cross_instance_readonly_field_rejected_spec_22_3_issue_3316) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _f: int; "
        "  public constructor(other: C) "
        "    Sink.IncrementByRef(ref other._f); "
        "  end constructor; "
        "end C; "
        "class Sink "
        "  public static method IncrementByRef(ref x: int) x := x + 1; "
        "  end IncrementByRef; "
        "end Sink; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "ref"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "readonly field `_f`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§22.3"));
}

// §9.1 / §9.4 / §22.8 SAME-INSTANCE gap: the existing positional-record
// fix narrowed only on the type axis (`currentType_ == fs->parent`), so a
// record R's OWN constructor could still write `other.X := 5` where
// `other` is a different R instance — an aliasing write through
// read-only storage the §22.8 ruling also closes. Post-fix: the same
// `isThisRootedLValueTarget` check narrows the positional-record carve
// out too. Note the LHS is `other.X`, not the bare `X := ...` (the
// bare spelling would be the legitimate §9.2 combined-form initializer;
// the cross-instance RHS read is allowed but the cross-instance LHS
// write is the defect).
BEZEL_TEST(readonly_per_instance_3316,
           record_ctor_cross_instance_positional_property_assignment_rejected_spec_9_1_issue_3316) {
    auto r = parseSemaAll(
        "module M; "
        "record R(X: int, Y: int) "
        "  public constructor(other: R) other.X := X + 1; end constructor; "
        "end R; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "auto-generated property `X`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "positional record"));
}

// §22.15 this-rooted birth-store rule (pure ctor): already rejected via
// PureChecker.cpp's `targetRootSymbol` walk, but the per-instance ruling
// makes it doubly-safe (sema rejects it on the §22.8 axis too, then
// purity rejects it on the §22.15 axis — either alone is sufficient).
// Pin the §22.8 case so it cannot regress (PureChecker is run below
// for this row).
BEZEL_TEST(readonly_per_instance_3316,
           pure_ctor_cross_instance_readonly_field_assignment_rejected_spec_22_15_issue_3316) {
    auto r = bezel::test::parseAndCollect(
        "module M; "
        "class C "
        "  public readonly _f: int; "
        "  public pure constructor(other: C) other._f := 1; end constructor; "
        "end C;");
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
        bezel::sema::checkPureBodies(*r.sourceFile, *r.ctx);
    }
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // §22.8 or §22.15 fires — either alone is correct. Pin at least one.
    bool has22_8 = hasMsg(r, "§22.8") || hasMsg(r, "declaring class's constructor");
    bool has22_15 = hasMsg(r, "§22.15") ||
                    hasMsg(r, "birth-store") ||
                    hasMsg(r, "target root");
    BEZEL_EXPECT_TRUE(has22_8 || has22_15);
}

// =========================================================================
// §22.8 / §23.1 (v0.270, #3299/#3316) — ACCEPT CONTROLS
// =========================================================================

// CONTROL: the explicit `this._f := e` spelling in a constructor — the
// §22.8 per-instance ruling's headline ACCEPT. Must keep compiling.
BEZEL_TEST(readonly_per_instance_3316,
           ctor_this_rooted_readonly_field_assignment_accepted_spec_22_8) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _f: int; "
        "  public constructor() this._f := 7; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: the bare-in-scope `_f := e` spelling in a constructor — the
// §22.8 per-instance ruling's other ACCEPT, the bare-implicit-this
// spelling (§22.8: "the bare in-scope spelling that resolves to `this`'s
// storage"). Must keep compiling.
BEZEL_TEST(readonly_per_instance_3316,
           ctor_bare_implicit_this_readonly_field_assignment_accepted_spec_22_8) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public readonly _f: int; "
        "  public constructor() _f := 7; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// FIX #5638 (§22.8, v0.334): a static readonly field is assignable only
// in a static constructor — an instance constructor writing a static field
// is a compile error regardless of spelling (`C._s` or bare `_s`).
BEZEL_TEST(readonly_per_instance_3316,
           ctor_type_qualified_static_readonly_assignment_rejected_spec_22_8_v0_334) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static readonly _s: int; "
        "  public constructor() C._s := 7; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    auto& msg = r.diagnostics.diagnostics()[0].message;
    BEZEL_EXPECT_TRUE(msg.find("22.8") != std::string::npos);
}

// FIX #5638 (§22.8, v0.334): bare static `_s := 7` from an instance
// constructor is the bare-implicit-this spelling of the same illegal store.
// Both spellings must be rejected.
BEZEL_TEST(readonly_per_instance_3316,
           ctor_bare_static_readonly_assignment_rejected_spec_22_8_v0_334) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static readonly _s: int; "
        "  public constructor() _s := 7; end constructor; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    auto& msg = r.diagnostics.diagnostics()[0].message;
    BEZEL_EXPECT_TRUE(msg.find("22.8") != std::string::npos);
}