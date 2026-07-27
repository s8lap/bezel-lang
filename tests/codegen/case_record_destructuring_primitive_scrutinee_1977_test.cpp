// fix #1977 / §19.3 / §9.4 (v0.194+): a record-destructuring pattern over a
// VALUE-TYPE PRIMITIVE scrutinee (e.g. `case (n: int) of Pt(a, b) => …`) aborts
// the COMPILER at `CreateExtractValue` with an LLVM GEP-type assertion: the
// scrutinee is an `i32`, not the `Pt` struct value, so the field-extract
// operand indices are invalid for the scalar LLVM type.
//
// §19.3 / §9.4: destructuring patterns describe a record VALUE. A primitive
// scrutinee can never be that record — semantically a "compile-time false"
// match (dispatch falls through to `_`). The backend must NOT abort; the
// arm body still has to lower (its identifier reads against the binding
// slots stay well-typed), and `case.end` must stay terminated.
//
// The pointer-branch fix (#1949) gated the `emitIsA` walk on `objectLike` (a
// string / runtime-managed reference cannot be a record), forced the is-a
// to a compile-time false, and entered a separate `unboxBB` to emit the
// binding slots (so the arm body lowers even though statically dead). The
// non-pointer branch in this issue is the sibling for a value-type
// PRIMITIVE / enum / inline-array scrutinee: the existing code skipped the
// pointer branch (`scrut->getType()->isPointerTy()` is false for an `i32`)
// and fell into the unconditional `CreateExtractValue` path. The fix adds
// a `scrut->getType()->isStructTy()` guard: when false, emit each binding
// slot with an `undef` placeholder of the field's LLVM type (so the arm
// body's reads resolve to well-typed allocas) and statically no-match
// (`getInt1(false)`).
//
// Sibling of #1949's `case_concrete_type_string_1938` test in structure:
// the AOT oracle runs the program and reports the exit code, which here is
// the `_`-arm value (9) for a primitive scrutinee. Without the fix the
// compiler aborts and the AOT process never runs (`run()` returns -999).
// The "genuine match" guard test pins a record scrutinee to keep the field-
// extract path exercised (no false negatives).

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Primitive int scrutinee against a record destructuring. Pre-#1977 the
// backend aborted at CreateExtractValue with an LLVM GEP assertion; the
// fix statically no-matches, the `_` arm runs, Main returns 9.
const char* kPrimitiveScrutinee =
    "module M; "
    "record Pt(x: int, y: int); "
    "class Program "
    "  public static method Main(): int "
    "    var n: int := 5; "
    "    return (case n of Pt(a, b) => a + b, _ => 9); "
    "  end Main; "
    "end Program; ";

// Boolean primitive scrutinee — same shape, different primitive type. Pins
// the fix is gated on isStructTy (not just on i32 specifically): a `bool`
// is i1 and equally non-struct.
const char* kBoolScrutinee =
    "module M; "
    "record Pt(x: int, y: int); "
    "class Program "
    "  public static method Main(): int "
    "    var b: bool := true; "
    "    return (case b of Pt(a, b) => a + b, _ => 7); "
    "  end Main; "
    "end Program; ";

// Float primitive scrutinee — covers f64 / f32, which are also non-struct.
const char* kFloatScrutinee =
    "module M; "
    "record Pt(x: int, y: int); "
    "class Program "
    "  public static method Main(): int "
    "    var f: float := 1.5f; "
    "    return (case f of Pt(a, b) => a + b, _ => 11); "
    "  end Main; "
    "end Program; ";

// GENUINE-MATCH CONTROL: a real Pt scrutinee must still match and bind the
// fields (3 + 4 = 7). Pins the fix does not over-reject by gating on the
// primitive case too aggressively.
const char* kGenuineMatch =
    "module M; "
    "record Pt(x: int, y: int); "
    "class Program "
    "  public static method Main(): int "
    "    var p := new Pt(3, 4); "
    "    return (case p of Pt(a, b) => a + b, _ => 9); "
    "  end Main; "
    "end Program; ";

}  // namespace

// ---------------------------------------------------------------------------
// (a) THE KEYSTONE REPRO for #1977. Pre-fix the backend aborted at
// CreateExtractValue (LLVM GEP assertion); `runAot` never produced a
// process (or produced one that died on the malformed IR), and `run()`
// returns -999. With the fix the program compiles, the `Pt(a, b)` arm is
// statically dead, the `_` arm runs, Main returns 9.
// ---------------------------------------------------------------------------

BEZEL_TEST(case_record_destructuring_primitive_scrutinee_1977,
           int_scrutinee_against_record_destructuring_compiles_spec_19_3) {
    BEZEL_EXPECT_EQ(run(std::string(kPrimitiveScrutinee)), 9);
}

// ---------------------------------------------------------------------------
// (b) Sibling primitive-type variant — `bool` is i1 and also non-struct.
// Pins the gate is on isStructTy (the issue's root cause), not on i32
// specifically.
// ---------------------------------------------------------------------------

BEZEL_TEST(case_record_destructuring_primitive_scrutinee_1977,
           bool_scrutinee_against_record_destructuring_compiles_spec_19_3) {
    BEZEL_EXPECT_EQ(run(std::string(kBoolScrutinee)), 7);
}

// ---------------------------------------------------------------------------
// (c) Floating-point primitive — f64 is also a non-struct LLVM type. The
// same gate rejects it.
// ---------------------------------------------------------------------------

BEZEL_TEST(case_record_destructuring_primitive_scrutinee_1977,
           float_scrutinee_against_record_destructuring_compiles_spec_19_3) {
    BEZEL_EXPECT_EQ(run(std::string(kFloatScrutinee)), 11);
}

// ---------------------------------------------------------------------------
// (d) OVER-REJECT GUARD — a real Pt scrutinee still matches and binds.
// Without this the fix could be wrong by gating too aggressively on
// primitive types; the record-typed scrutinee must take the destructuring
// arm, not the `_` arm (3 + 4 = 7).
// ---------------------------------------------------------------------------

BEZEL_TEST(case_record_destructuring_primitive_scrutinee_1977,
           record_scrutinee_destructuring_still_matches_spec_19_3) {
    BEZEL_EXPECT_EQ(run(std::string(kGenuineMatch)), 7);
}