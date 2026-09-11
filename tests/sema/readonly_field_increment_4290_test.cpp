// Spec-derived tests for fix #4290 — §17.6 "The increment and decrement
// operators" / §22.8 "Field initializers" (v0.298).
//
// §22.8, verbatim: "A field declared `readonly` may be assigned only in a
// field initializer or in a constructor body; assignment anywhere else is a
// compile error." ... "*Assignment* here means every store form — `:=`,
// compound assignment, `++`/`--` (§17.6), and binding the field to a `ref`
// or `out` parameter, which is the same store by other syntax the paragraph
// above already names."
// §17.6, verbatim: "The operand must be an assignable lvalue (a local
// variable, a writable field, an indexer slot, or a property with a setter
// — the same storage categories §22.3's `ref`/`out` arguments reuse).
// Read-only locations cannot be incremented."
//
// Filed by the lane implementing fix #4276 (readonly-derived-ctor
// reassignment) and independently reproduced by that fix's reviewer:
// `_f++` / `_f--` on a plain `readonly` FIELD, from an ORDINARY
// (non-constructor) method, compiled with NO diagnostic and mutated the
// field at runtime — measured `bezelc -o ... ; echo $?` -> 0 (no
// diagnostic), then the produced binary exiting 2 (the field's initial
// value 1, incremented). The `:=` and `+=` spellings of the identical store
// were ALREADY correctly rejected; only `++`/`--` reached no check.
//
// Root cause: `IncrementStmt`'s only field-shaped readonly handling was (a)
// a `const`-LOCAL check (LocalSymbol only, not FieldSymbol) and (b) a
// positional-record auto-generated-property check (`fs->posParam &&
// fs->parent->kind == Record`); `checkPropertyWriteTarget` was also called
// unconditionally, but that function's `dynamic_cast<PropertySymbol*>`
// fails for a plain `FieldSymbol` target and it early-returns (`if (!ps)
// return;`) — so an ORDINARY class field's `++`/`--` reached NO readonly
// check at all.
//
// Fix: `checkReadonlyAssignment`'s field-readonly logic (the
// construction-window carve-out plus the `readonly`-modifier rejection) is
// factored into a new shared helper, `checkReadonlyFieldWriteTarget` —
// the same shape fix #2868 already used to share `checkPropertyWriteTarget`
// between the `AssignmentStmt` and `IncrementStmt` arms — and the
// `IncrementStmt` arm now calls it alongside its existing
// `checkPropertyWriteTarget` call. This is the FIELD sibling of #2868's
// PROPERTY fix, not a fourth spelling of the carve-out: there is still
// exactly ONE implementation of "is this readonly field write inside its
// assignable context", now consulted from both statement forms instead of
// only one of them.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// REJECT — the issue's own repro and its siblings.
// =====================================================================

// The issue's own measured shape: bare `_f++` on a readonly field from an
// ordinary (non-constructor) method.
BEZEL_TEST(readonly_field_increment_4290,
           bare_postfix_increment_on_readonly_field_rejected_spec_22_8_17_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private readonly _f: int := 1; "
        "  public method Bump() "
        "    _f++; "
        "  end Bump; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "cannot increment/decrement readonly field `_f`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// The `--` twin of the row above — §17.6 governs both operators
// identically ("`++` and `--` are postfix-only... Read-only locations
// cannot be incremented" applies to `--` by the same rule).
BEZEL_TEST(readonly_field_increment_4290,
           bare_postfix_decrement_on_readonly_field_rejected_spec_22_8_17_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private readonly _f: int := 1; "
        "  public method Dec() "
        "    _f--; "
        "  end Dec; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "cannot increment/decrement readonly field `_f`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// The explicit `this.` spelling of the identical store — proves the fix
// covers both the bare-identifier and `this.`-qualified target shapes
// `checkReadonlyFieldWriteTarget` resolves (both spellings share ONE
// FieldSymbol resolution via `ctx_.exprSymbol`).
BEZEL_TEST(readonly_field_increment_4290,
           this_dot_postfix_increment_on_readonly_field_rejected_spec_22_8_17_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private readonly _f: int := 1; "
        "  public method Bump() "
        "    this._f++; "
        "  end Bump; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "cannot increment/decrement readonly field `_f`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// A `static readonly` field incremented from a `static` (non-constructor)
// method — the static-field twin of the instance-field repro, exercising
// the `isStatic` branch of the carve-out through the SAME shared helper.
BEZEL_TEST(readonly_field_increment_4290,
           static_readonly_field_increment_from_static_method_rejected_spec_22_8_17_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private static readonly _count: int := 1; "
        "  public static method Bump() "
        "    _count++; "
        "  end Bump; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "cannot increment/decrement readonly field `_count`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// The per-instance narrowing twin (§22.8: "an *instance* `readonly` field
// is assignable only through an lvalue chain rooted at `this`... so a
// constructor of the declaring type may **not** write another value's
// `readonly` storage held in a parameter, local, or field"): inside the
// declaring type's OWN constructor, incrementing ANOTHER instance's
// readonly field must still be rejected — `isThisRootedLValueTarget`
// returns false for `other._f` (chain base is the parameter `other`, not
// `this`), exactly as the `:=` spelling already requires.
BEZEL_TEST(readonly_field_increment_4290,
           readonly_field_increment_via_another_instance_inside_declaring_ctor_rejected_spec_22_8_17_6) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  public readonly _f: int; "
        "  public constructor(other: Holder) "
        "    other._f++; "
        "  end constructor; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "cannot increment/decrement readonly field `_f`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// CONTROL, unaffected by this fix: the `+=` spelling of the identical store
// was ALREADY rejected before this fix (`checkReadonlyAssignment` is
// called uniformly for every `AssignOp`, `+=` included, from the
// AssignmentStmt arm — this fix touches only the IncrementStmt arm). This
// row is the measurement that `++`/`--` was the ODD ONE OUT, not evidence
// that the whole compound-assignment family was broken.
BEZEL_TEST(readonly_field_increment_4290,
           compound_assignment_plus_equals_on_readonly_field_rejected_control_spec_22_8) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  private readonly _f: int := 1; "
        "  public method Bump() "
        "    _f += 1; "
        "  end Bump; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "cannot assign to readonly field `_f` outside the declaring "
           "class's constructor"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§22.8"));
}

// CONTROL, unaffected by this fix: a `readonly` AUTO-PROPERTY's `++` was
// ALREADY rejected before this fix, through `checkPropertyWriteTarget`
// (fix #2868) — the `PropertySymbol` arm this fix does not touch. Pinning
// it here proves the new `checkReadonlyFieldWriteTarget` call added
// alongside `checkPropertyWriteTarget` in the IncrementStmt arm does not
// change (or double-report on) the property case.
BEZEL_TEST(readonly_field_increment_4290,
           readonly_auto_property_increment_still_rejected_unaffected_control_spec_17_6_23_1) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  public readonly property N: int := 1; "
        "  public method Bump() "
        "    N++; "
        "  end Bump; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "cannot increment/decrement readonly property `N`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§17.6"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.1"));
}

// =====================================================================
// ACCEPT controls (LANE_RULES §15) — legal shapes adjacent to the new
// rejection must keep compiling AND keep producing the correct runtime
// value, not merely "did not reject".
// =====================================================================

// `++` on a NON-readonly field, bare spelling — must still compile and
// mutate at runtime exactly as before this fix.
BEZEL_TEST(readonly_field_increment_4290,
           postfix_increment_on_non_readonly_field_bare_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Holder "
        "  private _f: int := 1; "
        "  public method Bump() "
        "    _f++; "
        "  end Bump; "
        "  public method Get(): int "
        "    return _f; "
        "  end Get; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  h.Bump(); "
        "  return h.Get(); "
        "end Main; end C;"), 2);
}

// `++` on a readonly field INSIDE the declaring type's own constructor,
// as the field's single assignment (no declaration initializer — `int`
// defaults to 0, so the read half of `_f++`'s desugaring sees a definite
// value and the increment is the field's only store) — must compile and
// produce the incremented value.
BEZEL_TEST(readonly_field_increment_4290,
           postfix_increment_on_readonly_field_inside_declaring_constructor_single_assignment_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Holder "
        "  private readonly _f: int; "
        "  public constructor() "
        "    _f++; "
        "  end constructor; "
        "  public method Get(): int "
        "    return _f; "
        "  end Get; "
        "end Holder; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  return h.Get(); "
        "end Main; end C;"), 1);
}

// `++` on a plain LOCAL — the unaffected baseline case; `IncrementStmt`'s
// `ctx_.exprSymbol` resolves a `LocalSymbol`, so
// `checkReadonlyFieldWriteTarget`'s `dynamic_cast<FieldSymbol*>` fails and
// it is a no-op.
BEZEL_TEST(readonly_field_increment_4290,
           postfix_increment_on_local_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class C public static method Main(): int "
        "  var n := 1; "
        "  n++; "
        "  return n; "
        "end Main; end C;"), 2);
}

// `++` on a NON-readonly field of ANOTHER object, through a qualified
// `h._count++` chain from a different type's method — must keep
// compiling and mutating the OTHER instance's storage, proving the new
// FieldSymbol resolution added to the IncrementStmt arm does not
// misfire on a qualified receiver that is not `this`.
BEZEL_TEST(readonly_field_increment_4290,
           postfix_increment_on_non_readonly_field_of_another_object_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Holder "
        "  public _count: int := 5; "
        "end Holder; "
        "class Toucher "
        "  public static method Bump(h: Holder) "
        "    h._count++; "
        "  end Bump; "
        "end Toucher; "
        "class C public static method Main(): int "
        "  var h := new Holder(); "
        "  Toucher.Bump(h); "
        "  return h._count; "
        "end Main; end C;"), 6);
}
