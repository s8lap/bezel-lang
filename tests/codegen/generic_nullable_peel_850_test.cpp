// fix #850 / §14.1 / §14.2: generic inference binds T = int? instead of int
// when the parameter is `T?` and the argument is `int?`.
//
// Root cause: unifyArgSubst (sema, ExprTypecheck_Calls.cpp) and
// unifyMethodReturnTypeArg (codegen, LlvmBackend_Calls.cpp) both bound the
// whole nullable argument type to T — so `where T: value` failed (int? is not
// a value type) and the return type `T` stayed nullable. When both nullables
// are present (parameter `T?`, argument `int?`), the `?` is structural and the
// two cancel: T = int.
//
// Additionally, lowerCoalesce (LlvmBackend_Exprs.cpp) did not recognise
// `T?` with T bound to a value type as a nullable VALUE coalesce — it fell
// through to the pointer-null branch, producing 0 instead of the payload.
//
// §14.1: inference from a T? parameter with a nullable argument peels the `?`.
// §14.2: where T: value constraint checked against the PEELED T (int satisfies,
//         int? does not).
//
// Authority: spec §14.1 (generic type argument inference), §14.2 (constraints),
// issue #850.

#include "support/codegen_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") != std::string::npos)
            return true;
    return false;
}

const char* kUnwrap =
    "module M; "
    "static class U "
    "  public static method Unwrap<T>(v: T?): T where T: value "
    "    return v ?? default(T); "
    "  end Unwrap; "
    "end U; ";

}  // namespace

// §14.1 / #850: the issue repro — Unwrap<T>(v: T?) called with int? = 42 must
// return 42, not crash/miscompile. T is inferred as int (not int?), so
// `where T: value` holds and the return type is int.
BEZEL_TEST(generic_nullable_peel_850, unwrap_int_nullable_returns_payload) {
    BEZEL_EXPECT_EQ(runFull(std::string(kUnwrap) +
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return U.Unwrap(x); "
        "  end Main; "
        "end C;"), 42);
}

// §14.1 / #850: null int? argument returns default(int) = 0.
BEZEL_TEST(generic_nullable_peel_850, unwrap_null_returns_default) {
    BEZEL_EXPECT_EQ(runFull(std::string(kUnwrap) +
        "class C "
        "  public static method Main(): int "
        "    var x: int? := null; "
        "    return U.Unwrap(x); "
        "  end Main; "
        "end C;"), 0);
}

// §14.1 / #850: the result is genuinely int (arithmetic works).
BEZEL_TEST(generic_nullable_peel_850, unwrap_result_participates_in_arithmetic) {
    BEZEL_EXPECT_EQ(runFull(std::string(kUnwrap) +
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 7; "
        "    return U.Unwrap(x) * 6; "  // 42
        "  end Main; "
        "end C;"), 42);
}

// §14.2 / #850 regression guard: a BARE `T` parameter (not `T?`) given an
// int? argument must still bind T = int? and be REJECTED by `where T: value`.
// The peel must NOT fire when `pt->nullable == false`.
BEZEL_TEST(generic_nullable_peel_850, bare_T_with_nullable_arg_still_rejected) {
    // Bare T parameter: `method G<T>(v: T): T where T: value`
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method G<T>(v: T): T where T: value "
        "    return v; "
        "  end G; "
        "end F; "
        "class C "
        "  public static method Main(): int "
        "    var x: int? := 42; "
        "    return F.G(x); "   // bare T, int? arg → T = int? → violates value
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}
