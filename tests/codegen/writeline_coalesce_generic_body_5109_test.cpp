// Regression test for #5109 — `Console.WriteLine(x ?? d)` with `x: T?` (a
// value `T`, e.g. `int`) inside a generic method or constructor body passed a
// raw nullable struct to `bz_string_to_utf8z`.
//
// WHAT THE SPEC REQUIRES. §5.1 (v0.307) "Nullable conversions" / the `??`
// null-coalescing rule: `a ?? b` for `a: T?` and `b: T` has result type `T`
// (the non-nullable form), which — per §8.5's universal rendering — converts
// to a string for `Console.WriteLine`.
//
// ROOT CAUSE (codegen only; sema already typed `??`'s result correctly as
// the non-nullable `T`). `lowerCoalesce` (LlvmBackend_Exprs2.cpp) resolves a
// bare, unresolved `T?` LHS to its concrete nullable form via
// `nullableTypeParamConcrete`, which reads the ACTIVE `genericSubstSemaTy_`
// substitution — populated only while compiling the MONOMORPHIZED
// (specialized) body. Every generic method/constructor ALSO keeps an
// unspecialized, i32-erased TEMPLATE version, compiled with no active
// substitution and IR-VERIFIED alongside the specialization even when the
// template is never called by a program that only invokes the specialized
// instantiation. In that template compile, `nullableTypeParamConcrete`
// legitimately fails to resolve (no substitution), `isNullableValueTy`
// answers false for the still-bare `T?`, and lowerCoalesce fell to its
// "#1045 non-pointer, non-nullable-value LHS" arm — WRONG here, because the
// template's own LLVM value for a bare-generic-typed `T?` is STILL the
// tagged `{i8 hasValue; T}` struct (T erased to its i32 placeholder), not a
// "can never be null" scalar. The arm returned the raw struct as the `??`
// result; `Console.WriteLine`'s argument-to-string dispatch has no case for
// a struct-valued, unresolved bare-generic-typed argument, so it fell
// through to `bz_string_to_utf8z` unconverted — a malformed call that fails
// IR verification for the WHOLE MODULE (the template function is still
// compiled and verified, only never executed).
//
// THE FIX teaches `lowerCoalesce` to recognize the nullable-value carrier
// STRUCTURALLY (a 2-field `{i8, X}` struct whose first field is the
// hasValue-tag type) in addition to via sema resolution — `nullablePrimHas
// Value`/`nullablePrimValue` are pure `extractvalue`s that need no concrete
// sema type to be correct, so the unwrap now also fires in the unspecialized
// template.
//
// SPEC-CITED TEST CASES. Each case is on stdout (byte-for-byte; runAotOut is
// the value channel for observables that don't fit an 8-bit exit code):
//   1. generic method, non-null `x: T?` at T=int → prints the payload (5).
//   2. generic method, null `x: T?` at T=int → prints the default (1).
//   3. generic constructor, non-null/null at T=int (the #5089-sibling shape).
//   4. a string-`T` control (T=string) — the argument was already a string,
//      and must remain unaffected by the struct-carrier fix.
//   5. a non-generic control (`int?`, no generic body at all) — the
//      pre-existing, always-correct behaviour must stay exact.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

const char* kMethodProgram =
    "module M; "
    "class P "
    "  private static method Show<T>(x: T?, d: T) "
    "    Console.WriteLine(x ?? d); "
    "  end Show; "
    "  public static method Main(): int "
    "    var a: int? := 5; "
    "    var b: int? := null; "
    "    Show<int>(a, 1); "
    "    Show<int>(b, 1); "
    "    return 0; "
    "  end Main; "
    "end P;";

const char* kCtorProgram =
    "module M; "
    "class Box<T> "
    "  public constructor(x: T?, d: T) Console.WriteLine(x ?? d); end constructor; "
    "end Box; "
    "class P "
    "  public static method Main(): int "
    "    var a: int? := 5; "
    "    var b: int? := null; "
    "    var boxA := new Box<int>(a, 1); "
    "    var boxB := new Box<int>(b, 1); "
    "    return 0; "
    "  end Main; "
    "end P;";

const char* kStringControlProgram =
    "module M; "
    "class P "
    "  private static method Show<T>(x: T?, d: T) "
    "    Console.WriteLine(x ?? d); "
    "  end Show; "
    "  public static method Main(): int "
    "    var a: string? := \"hi\"; "
    "    var b: string? := null; "
    "    Show<string>(a, \"fallback\"); "
    "    Show<string>(b, \"fallback\"); "
    "    return 0; "
    "  end Main; "
    "end P;";

const char* kNonGenericControlProgram =
    "module M; "
    "class P "
    "  private static method Show(x: int?, d: int) "
    "    Console.WriteLine(x ?? d); "
    "  end Show; "
    "  public static method Main(): int "
    "    var a: int? := 5; "
    "    var b: int? := null; "
    "    Show(a, 1); "
    "    Show(b, 1); "
    "    return 0; "
    "  end Main; "
    "end P;";

}  // namespace

// §5.1 / #5109: a generic METHOD's `x ?? d` at T=int, non-null and null.
BEZEL_TEST(writeline_coalesce_generic_body_5109, method_non_null_and_null) {
    BEZEL_EXPECT_EQ(runAotOut(kMethodProgram), "5\n1");
}

// §5.1 / #5109 (sibling of #5089): a generic CONSTRUCTOR's `x ?? d` at T=int.
BEZEL_TEST(writeline_coalesce_generic_body_5109, ctor_non_null_and_null) {
    BEZEL_EXPECT_EQ(runAotOut(kCtorProgram), "5\n1");
}

// #5109 control: T=string (already a reference type; must be unaffected).
BEZEL_TEST(writeline_coalesce_generic_body_5109, string_t_control) {
    BEZEL_EXPECT_EQ(runAotOut(kStringControlProgram), "hi\nfallback");
}

// #5109 control: non-generic `int?` coalesce (no template/specialization
// split at all) must keep printing exactly as before the fix.
BEZEL_TEST(writeline_coalesce_generic_body_5109, non_generic_control) {
    BEZEL_EXPECT_EQ(runAotOut(kNonGenericControlProgram), "5\n1");
}
