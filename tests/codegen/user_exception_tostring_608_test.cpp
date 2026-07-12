// Codegen tests for issue #608 — `e.ToString()` on a user-defined exception
// class (e.g. `class ErrA: Exception`) returns garbage instead of the
// qualified type name.
//
// Root cause (two-part):
//
// (a) Sema (typeCall): In typeCall() in ExprTypecheck_Calls.cpp, when a user
//     class type has no MethodSymbol for `ToString` (because
//     TypeSymbol::baseTypes does not include built-in exception bases), sema
//     did not set the CallExpr type for `e.ToString()`.  The existing code
//     only covered Object-primitive and interface receivers.  Fix: extend the
//     `ToString` → `string` CallExpr typing to also cover Named-class receivers
//     (all user-defined classes, including user exception classes).
//     per §8.5 (v0.134).
//
// (b) Codegen (lowerCall): In lowerCall(), the `if (!ms)` block dispatched
//     `ToString` through `lowerObjectToString` only for Object-primitive or
//     interface-typed receivers.  A user class type (Named TypeRef with a
//     non-null Class declaration) was not included, so the call fell through
//     to `return nullptr` → `var s` received null → `s.Length` read garbage.
//     Fix: extend the condition to also include Named Class receivers.
//     per §8.5 (v0.134).
//
// The fix is correct because every user class already has a correctly-wired
// vtable slot 3 (classToStringThunk in LlvmBackend_Setup.cpp) that returns
// the qualified type name.  lowerObjectToString dispatches through that slot.
//
// Tests organised into five groups:
//
//   1. Basic ToString on user exception class — returns non-empty type name.
//   2. ToString on user exception via Exception-typed variable.
//   3. ToString on two-level user hierarchy (ErrB: ErrA: Exception).
//   4. User exception with user-defined ToString override.
//   5. Sema — no "no member" error for ToString on user exception.
//   6. Regression: #606/#607/#611 — Message/catch via builtin base.
//
// per §8.5 (ToString), §12 (inheritance), §31 (exceptions), v0.134.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Returns true iff the run produced an error diagnostic containing `keyword`.
bool hasError(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
                d.message.find(keyword) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// ── Group 1: Basic ToString on user exception class ───────────────────────────

// §8.5 / §12 / §31: `e.ToString()` on a user exception returns a non-empty
// string (the qualified type name, e.g. "ErrA" — length > 0).
// Before fix: lowerCall returned null → s uninitialized → s.Length = 0.
// After fix: vtable slot 3 dispatch returns type-name string, length > 0.
// Note: multi-statement catch arm body requires `do ... end` syntax (§31.2.2).
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, user_exception_tostring_nonzero_length_spec_8_5) {
    int len = run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"hello\"); "
        "  catch var e: ErrA => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(len > 0, true);   // type name must be non-empty
}

// §8.5 / §12 / §31: `e.ToString()` length is sane (< 200) — sanity-checks
// that we don't get a huge garbage value like 32764.
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, user_exception_tostring_sane_length_spec_8_5) {
    int len = run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"hello\"); "
        "  catch var e: ErrA => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(len < 200, true);
}

// §8.5 / §12 / §31: no-arg user exception ToString also returns non-empty string.
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, user_exception_no_arg_tostring_nonzero_spec_8_5) {
    int len = run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor() end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(); "
        "  catch var e: ErrA => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(len > 0, true);
}

// §8.5 / §12 / §31: `e.ToString()` on a differently-named exception class.
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, user_exception_appError_tostring_nonzero_spec_8_5) {
    int len = run(
        "module M; "
        "class AppError: Exception "
        "  public constructor(msg: string) base(msg); end constructor; "
        "end AppError; "
        "class C public static method Main(): int "
        "  try "
        "    throw new AppError(\"bad\"); "
        "  catch var e: AppError => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(len > 0, true);
}

// ── Group 2: ToString via Exception-typed variable ────────────────────────────

// §8.5 / §12 / §31: catching as `Exception`, calling `e.ToString()` on the
// base-typed variable must also return a non-empty string.
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, user_exception_tostring_via_exception_var_spec_8_5) {
    int len = run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"hi\"); "
        "  catch var e: Exception => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(len > 0, true);
}

// ── Group 3: Two-level user hierarchy ─────────────────────────────────────────

// §8.5 / §12 / §31: two-level user exception hierarchy (ErrB: ErrA: Exception).
// `e.ToString()` on the leaf type must return non-empty.
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, two_level_hierarchy_tostring_nonzero_spec_8_5_12) {
    int len = run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: ErrA "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"deep\"); "
        "  catch var e: ErrB => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(len > 0, true);
}

// §8.5 / §12: two-level hierarchy — catch as base (ErrA), call ToString.
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, two_level_hierarchy_tostring_via_user_base_spec_8_5_12) {
    int len = run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class ErrB: ErrA "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrB; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrB(\"chain\"); "
        "  catch var e: ErrA => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(len > 0, true);
}

// ── Group 4: User exception with a user-defined ToString override ─────────────

// §8.5 / §12: if the user class defines its own `ToString` override, it must
// be called (not the default type-name thunk).  Since the user override is a
// real MethodSymbol, this test confirms the override path is unaffected.
// The override returns "custom".Length = 6.
// Expression-body `=> "custom"` avoids needing an explicit `end ToString;`.
// per §8.5 / §12.2 (method override), v0.134.
BEZEL_TEST(codegen_608, user_exception_tostring_user_override_spec_8_5_12_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "  public override method ToString(): string => \"custom\"; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"ignored\"); "
        "  catch var e: ErrA => "
        "    return e.ToString().Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        6);   // "custom".Length = 6
}

// §8.5 / §12: same as above but using `do...end` multi-statement arm body to
// confirm the override works in any catch-arm form.
// per §8.5 / §12.2 (method override), v0.134.
BEZEL_TEST(codegen_608, user_exception_tostring_user_override_do_form_spec_8_5_12_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "  public override method ToString(): string => \"custom\"; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"ignored\"); "
        "  catch var e: ErrA => do "
        "    var s := e.ToString(); "
        "    return s.Length; "
        "  end "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        6);   // "custom".Length = 6
}

// §8.5 / §12: test the override via direct instantiation (no catch arm).
// Uses the multi-method class pattern: each block-body method ends with
// `end MethodName;` and the class ends with `end ErrA;`.
// per §8.5 / §12.2 (method override), v0.134.
BEZEL_TEST(codegen_608, user_exception_tostring_user_override_direct_spec_8_5_12_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "  public override method ToString(): string => \"custom\"; "
        "  public static method Test(): int "
        "    var e := new ErrA(\"x\"); "
        "    return e.ToString().Length; "
        "  end Test; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  return ErrA.Test(); "
        "end Main; end C;"),
        6);   // "custom".Length = 6
}

// ── Group 5: Sema — no "no member" error for ToString ─────────────────────────

// §8.5 / §12: sema must NOT emit "no member `ToString`" for a user exception
// class.  The universal-member suppression must silence any such diagnostic.
// per §8.5 (v0.134).
BEZEL_TEST(codegen_608, sema_no_member_error_for_tostring_spec_8_5) {
    BEZEL_EXPECT_EQ(
        hasError(
            "module M; "
            "class ErrA: Exception "
            "  public constructor(m: string) base(m); end constructor; "
            "end ErrA; "
            "class C public static method Main(): int "
            "  try "
            "    throw new ErrA(\"x\"); "
            "  catch var e: ErrA => do "
            "    var s := e.ToString(); "
            "    return s.Length; "
            "  end "
            "  end; "
            "  return 0; "
            "end Main; end C;",
            "no member"),
        false);   // must NOT have "no member" error
}

// ── Group 6: Regression — #606/#607/#611 still pass ──────────────────────────

// #606 regression guard: `e.Message` on a user-derived exception class
// must NOT generate a "no member" error — the builtin-inherited-member path
// must still work.  per §31 / stdlib Exception.Message.
BEZEL_TEST(codegen_608, regression_606_message_on_derived_class_spec_31) {
    BEZEL_EXPECT_EQ(
        hasError(
            "module M; "
            "class NetworkError: Exception "
            "  public constructor(m: string) base(m); end constructor; "
            "end NetworkError; "
            "class C public static method Main(): int "
            "  try "
            "    throw new NetworkError(\"not found\"); "
            "  catch var e: NetworkError => return e.Message.Length; "
            "  end; "
            "  return 0; "
            "end Main; end C;",
            "no member"),
        false);
}

// #607 regression guard: `e.Message` through an `Exception`-typed variable
// must work correctly at runtime.  per §31 / stdlib Exception.Message.
BEZEL_TEST(codegen_608, regression_607_message_via_exception_var_spec_31) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class NetworkError: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end NetworkError; "
        "class C public static method Main(): int "
        "  try "
        "    throw new NetworkError(\"not found\"); "
        "  catch var exc: Exception => return exc.Message.Length; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        9);   // "not found".Length = 9
}

// #611 regression guard: `catch var e: Exception` must catch a user-derived
// exception.  per §31.2 / §12 (ErrA IS-A Exception via descriptor chain).
BEZEL_TEST(codegen_608, regression_611_catch_user_ex_as_builtin_base_spec_31_12) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class ErrA: Exception "
        "  public constructor(m: string) base(m); end constructor; "
        "end ErrA; "
        "class C public static method Main(): int "
        "  try "
        "    throw new ErrA(\"x\"); "
        "  catch var e: ErrA => return 100; "
        "  catch var e: Exception => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        100);   // specific ErrA arm fires first (§31.2)
}
