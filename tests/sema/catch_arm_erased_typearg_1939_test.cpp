// Spec-derived tests for §31.2.1/§12.9/§14.2 (#1939 leg 3 guard, spec v0.220):
// the erasure guard on CATCH-ARM types — parity with #2006 (is/as), #2012
// (statement case) and #2018 (value-form case).
//
// §31.2.1: an arm "matches any thrown exception that is an instance of the
// type". A constructed generic arm type whose argument is a still-ERASED kind
// (a function type — appendDescKey mints no identity token, §14.2) reaches the
// COLLAPSED shared descriptor at dispatch, so `catch var e: MyExc<(int)->int>`
// would match a thrown `MyExc<(string)->string>` and bind it at a wrong static
// type — the #2001/#2006 confusion class, on the catch surface. Reject at
// compile time, exactly as the other three surfaces do. A bare type-parameter
// arm (`var ex: TEx`, §31.2.1's admitted form — #1505) and a
// parameter-MENTIONING arm (`MyExc<T>`) stay accepted: T resolves to a
// concrete argument at monomorphization (#1843; the T-pinned-to-erased
// residual is the design-gated #2012/#2069 scope).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
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

bool hasCatchErasedError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not runtime-distinguishable") != std::string::npos)
            return true;
    }
    return false;
}

std::string wrap(const std::string& body) {
    return "module M;\n"
           "class MyExc<T>: Exception\n"
           "  public constructor(msg: string) base(msg); end constructor;\n"
           "end MyExc;\n"
           "class Program\n" +
           body + "end Program;\n";
}

}  // namespace

// (1) Ground function-typed argument, BINDING arm. REJECTED (FAILS WITHOUT the
// fix: the arm compiled and dispatch reached the collapsed descriptor).
BEZEL_TEST(catch_arm_erased_typearg_1939, binding_function_arg_rejected_31_2_1) {
    auto r = parseSemaAll(wrap(
        "  public method M(): int\n"
        "    try\n"
        "      return 1;\n"
        "    catch\n"
        "      var e: MyExc<(int) -> int> => do return 2; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasCatchErasedError(r));
}

// (2) Ground function-typed argument, NON-binding arm — dispatch is by the
// same descriptor either way. REJECTED.
BEZEL_TEST(catch_arm_erased_typearg_1939,
           nonbinding_function_arg_rejected_31_2_1) {
    auto r = parseSemaAll(wrap(
        "  public method M(): int\n"
        "    try\n"
        "      return 1;\n"
        "    catch\n"
        "      MyExc<(int) -> int> => do return 2; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasCatchErasedError(r));
}

// (3) Reified argument stays ACCEPTED — `MyExc<int>` carries an identity token.
BEZEL_TEST(catch_arm_erased_typearg_1939, reified_arg_accepted_31_2_1) {
    auto r = parseSemaAll(wrap(
        "  public method M(): int\n"
        "    try\n"
        "      return 1;\n"
        "    catch\n"
        "      var e: MyExc<int> => do return 2; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasCatchErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (4) Bare type-parameter arm (§31.2.1 / #1505) stays ACCEPTED — matching is
// per instantiation against the substituted argument.
BEZEL_TEST(catch_arm_erased_typearg_1939, bare_type_parameter_accepted_31_2_1) {
    auto r = parseSemaAll(wrap(
        "  public method Probe<TEx>(action: () -> ()): int\n"
        "      where TEx: Exception\n"
        "    try\n"
        "      action();\n"
        "    catch\n"
        "      var ex: TEx => do return 1; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end Probe;\n"));
    BEZEL_EXPECT_FALSE(hasCatchErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (5) A parameter-MENTIONING constructed arm (`MyExc<T>`) stays ACCEPTED —
// the conservative-accept posture on type variables shared by every surface
// (the T-pinned-to-an-erased-kind residual is design-gated, #2012/#2069).
BEZEL_TEST(catch_arm_erased_typearg_1939,
           tparam_mentioning_arm_accepted_31_2_1) {
    auto r = parseSemaAll(wrap(
        "  public method Probe<T>(action: () -> ()): int\n"
        "    try\n"
        "      action();\n"
        "    catch\n"
        "      var ex: MyExc<T> => do return 1; end\n"
        "    end;\n"
        "    return 0;\n"
        "  end Probe;\n"));
    BEZEL_EXPECT_FALSE(hasCatchErasedError(r));
}
