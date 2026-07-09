// Spec-derived tests for issue #1303 — an inline `out var x` bound to a GENERIC
// method's non-nullable `out r: T` parameter must take the SUBSTITUTED type T
// (non-null), not an unresolved `?` (§22.4, §14.1).
//
// §22.4: `out var x` declares a new variable inline; "Its type is inferred from
// the method's `out` parameter declaration." For a generic method called with a
// concrete type argument, the out parameter's type at the call site is the
// SUBSTITUTED type (§14.1 — generics are monomorphized), so `out var x` against
// `out r: T` called as `G<int>(...)` binds `x: int` (non-null).
//
// THE BUG (#1303, a residual of CLOSED #1164): the inline out-var local was
// registered (ExprTypecheck_Exprs.cpp visitChildren) from the callee's FORMAL
// parameter `T` resolved in the CALLER's scope, where the method's type
// parameter is unbound, so the local carried an unresolved `T` (rendered `?`).
// The #1469 re-type in typeCall only re-typed the nullable VALUE-TYPE shape
// (`out T?` → `int?`, whose tagged-struct representation desyncs codegen's
// slot); it left the non-nullable `out T` → `int` and every reference-type
// shape as `?`, so arithmetic / member access on the out-var was spuriously
// rejected with "operator `+` is not defined for type `?`". This reproduced
// regardless of whether the call's result was consumed; only `int + ?` (left-
// operand-driven operator resolution) masked it. The runtime value was always
// correct (codegen sizes the slot from the formal parameter); only the static
// type was `?`.
//
// THE FIX (ExprTypecheck_Calls.cpp typeCall): the re-type now assigns the
// substituted concrete type whenever the formal type parameter is fully
// resolved (`concrete != resolved`, pointer inequality, and the result
// references no method type parameter), for EVERY shape — `T`→`int`,
// `T?`→`int?`, `T`→`string`, `T?`→`string?` — with the parameter's declared
// nullability preserved by substituteReturnType (neither over-narrowing nor
// over-widening). The codegen out-var slot sizer (LlvmBackend_Calls2.cpp
// ensureOutVarSlots) now prefers the sema-resolved concrete local type over
// the formal `T` (which lowers to the i32 placeholder, correct only for int),
// so a reference / wider-value out-var slot is sized correctly.
//
// Each test PINS the bug: without the fix the program is a spurious BAD_REJECT
// (runJit returns ran=false → -999); with the fix it compiles and the exit
// code is the spec-derived value, confirmed by AOT.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

// A generic copier whose `out r: T` parameter is written by `r := v` — the
// minimal §22.4 shape from issue #1303 (no return type, called as a bare
// expression statement, exactly the reported repro).
const char* kSetter =
    "module M; "
    "class C "
    "  public static method GetDefault<T>(v: T, out r: T) "
    "    r := v; "
    "  end GetDefault; "
    "end C; ";
}  // namespace

// The reported repro: a BARE-STATEMENT generic out-call, then arithmetic on the
// inline out-var. `out var x` is `int` (§22.4/§14.1); `x + 1` is valid and
// yields 77 + 1 = 78. Was a spurious "operator `+` is not defined for type `?`"
// BAD_REJECT before the fix.
BEZEL_TEST(generic_out_var_nonnull_1303, bare_statement_int_arithmetic) {
    BEZEL_EXPECT_EQ(run(
        std::string(kSetter) +
        "class D "
        "  public static method Main(): int "
        "    C.GetDefault<int>(77, out var x); "
        "    var y := x + 1; "
        "    return y; "
        "  end Main; "
        "end D;"), 78);
}

// Same shape with the type argument INFERRED from the value argument
// (`GetDefault(77, …)` binds T=int) — the out-var is `int` and `x + 1` works.
BEZEL_TEST(generic_out_var_nonnull_1303, inferred_type_arg) {
    BEZEL_EXPECT_EQ(run(
        std::string(kSetter) +
        "class D "
        "  public static method Main(): int "
        "    C.GetDefault(77, out var x); "
        "    var y := x + 1; "
        "    return y; "
        "  end Main; "
        "end D;"), 78);
}

// The out-var is genuinely non-null: arithmetic with the out-var as the LEFT
// operand (`s + 1`) requires a concrete type (left-operand operator resolution
// masked `int + ?` but not `? + int`). 5 + 1 = 6.
BEZEL_TEST(generic_out_var_nonnull_1303, out_var_as_left_operand) {
    BEZEL_EXPECT_EQ(run(
        std::string(kSetter) +
        "class D "
        "  public static method Main(): int "
        "    C.GetDefault<int>(5, out var s); "
        "    var y := s + 1; "
        "    return y; "
        "  end Main; "
        "end D;"), 6);
}

// A NON-generic `out r: int` (the working baseline the issue contrasts against)
// is unaffected — the out-var is `int` and `x + 1` yields 78. Guards against a
// regression on the non-generic path.
BEZEL_TEST(generic_out_var_nonnull_1303, non_generic_out_int_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method GetDefault(v: int, out r: int) "
        "    r := v; "
        "  end GetDefault; "
        "end C; "
        "class D "
        "  public static method Main(): int "
        "    C.GetDefault(77, out var x); "
        "    var y := x + 1; "
        "    return y; "
        "  end Main; "
        "end D;"), 78);
}

// Control: a GENUINELY-NULLABLE generic `out r: T?` value parameter KEEPS its
// nullability — `x = null` is well-typed and `x!` force-unwraps the delivered
// payload (§7). A fix that over-narrowed `T?` to `int` would make both
// `x = null` and `x!` ill-typed. (This shape was already handled by the #1469
// re-type; the #1303 fix must not disturb it.)
BEZEL_TEST(generic_out_var_nonnull_1303, nullable_out_value_stays_nullable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method GetDefault<T>(v: T, out r: T?) where T: value "
        "    r := v; "
        "  end GetDefault; "
        "end C; "
        "class D "
        "  public static method Main(): int "
        "    C.GetDefault<int>(77, out var x); "
        "    if x = null then return -1; end; "
        "    return x!; "  // 77
        "  end Main; "
        "end D;"), 77);
}

// A non-nullable generic `out r: T` bound to a REFERENCE type (string): the
// out-var is `string` (non-null) and bare `s.Length` resolves (§7.4) without a
// `!`. Before the codegen half of the fix the sema-typed local exposed a latent
// slot-sizing bug (the formal `T` lowered to an i32 placeholder) and
// `s.Length` crashed codegen with a malformed-IR GEP.
BEZEL_TEST(generic_out_var_nonnull_1303, reference_type_out_bare_member) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method CopyTo<T>(v: T, out r: T) "
        "    r := v; "
        "  end CopyTo; "
        "end C; "
        "class D "
        "  public static method Main(): int "
        "    C.CopyTo<string>(\"hi\", out var s); "
        "    if s.Length = 2 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end D;"), 1);
}

// A nullable generic `out r: T?` bound to a REFERENCE type (string): the out-var
// is `string?` (nullable), so a bare `s.Length` is ill-typed and `s = null` /
// `s!` are the correct forms (§7). Confirms the fix neither over-narrows
// (`T?`→`string`) nor over-widens for nullable reference types.
BEZEL_TEST(generic_out_var_nonnull_1303, nullable_reference_out_keeps_nullable) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method GetDefault<T>(v: T, out r: T?) "
        "    r := v; "
        "  end GetDefault; "
        "end C; "
        "class D "
        "  public static method Main(): int "
        "    C.GetDefault<string>(\"hi\", out var s); "
        "    if s = null then return -1; end; "
        "    if s!.Length = 2 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end D;"), 1);
}
