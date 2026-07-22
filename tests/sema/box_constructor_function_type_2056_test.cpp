// Spec-derived regression tests for §5.1 / §14.1 (v0.204), issue #2056.
// A constructor parameter declared with its class type parameter is checked after
// substituting the concrete type argument of the constructed type. Therefore the
// identity conversion admits `new Box<(int) -> int>(f)` when `f` has exactly that
// function type. A mismatched function signature, a function in an `int` slot,
// and a string in an `int` slot remain §5.1 errors.
//
// Before the fix, checkArgsAgainstParams compared the function payload against
// the unresolved declaration-side `U`. Its conservative function-to-nonfunction
// guard rejected the valid identity case instead of checking against the
// constructor call site's substituted `(int) -> int` parameter type.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasConstructorConvertError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("argument `v` to `new Box`") != std::string::npos &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("§5.1") != std::string::npos)
            return true;
    }
    return false;
}

constexpr std::string_view kBoxDecl =
    "module M; "
    "class Box<U> "
    "  public V: U; "
    "  public constructor(v: U) V := v; end constructor; "
    "end Box; ";

}  // namespace

// §5.1 identity + §14.1 (#2056): the concrete constructor parameter is
// `(int) -> int`, exactly the payload's type. FAILS without the fix because the
// checker sees unresolved `U`; PASSES with call-site class substitution.
BEZEL_TEST(box_constructor_function_type_2056,
           matching_function_payload_accepted_spec_5_1) {
    auto r = parseSemaAll(
        std::string(kBoxDecl) +
        "class C "
        "  public static method Id(x: int): int return x; end Id; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Id; "
        "    using var b := new Box<(int) -> int>(f); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.1.1 + §5.1: substitution must not over-accept a payload whose function
// parameter slot is incompatible with the concrete constructor parameter.
BEZEL_TEST(box_constructor_function_type_2056,
           mismatched_function_signature_rejected_spec_5_1) {
    auto r = parseSemaAll(
        std::string(kBoxDecl) +
        "class C "
        "  public static method Id(x: bool): int return (if x then 1 else 0); end Id; "
        "  public static method Main(): int "
        "    var f: (bool) -> int := Id; "
        "    using var b := new Box<(int) -> int>(f); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstructorConvertError(r));
}

// §5.1 / §14.1: the inverse mismatch remains rejected after substitution too —
// a function payload does not implicitly convert to the concrete `int` slot.
BEZEL_TEST(box_constructor_function_type_2056,
           function_payload_to_int_slot_rejected_spec_5_1) {
    auto r = parseSemaAll(
        std::string(kBoxDecl) +
        "class C "
        "  public static method Id(x: int): int return x; end Id; "
        "  public static method Main(): int "
        "    var f: (int) -> int := Id; "
        "    using var b := new Box<int>(f); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstructorConvertError(r));
}

// §5.1 / §14.1: substitution is not function-specific. A string payload does
// not implicitly convert to the concrete `int` constructor slot; checking the
// raw declaration-side `U` used to miss this mismatch entirely.
BEZEL_TEST(box_constructor_function_type_2056,
           string_payload_to_int_slot_rejected_spec_5_1) {
    auto r = parseSemaAll(
        std::string(kBoxDecl) +
        "class C public static method Main(): int "
        "  using var b := new Box<int>(\"not an int\"); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasConstructorConvertError(r));
}
