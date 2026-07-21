// fix #2067 / §5.1 / §27.1.1 (v0.204): a scalar/non-function source is NEVER
// implicitly convertible to a function-typed target. There is no scalar-to-
// function conversion in the §5.1 identity/widening/integer-promotion/
// reference/numeric/null enumeration. The §27.6 property-getter promotion
// (zero-argument function whose return matches the property's declared type)
// is the only exception; every other non-function → function pair was
// previously silently accepted and reached codegen, producing a malformed
// LLVM call whose scalar argument did not match the callee's pointer /
// function-value parameter.
//
// Repro that triggered the malformed-IR verifier abort:
//   class C { static Take(f: (int) -> int): int return 0; end;
//             static Main(): int return Take(42); end; }
// emitted  `call i32 @"bz$C$Take"(i32 42)` against a `ptr`-parameter callee.
//
// After the fix, checkAssignable rejects the non-function → function pair
// BEFORE the Function-to-Function block, with a clean §5.1 diagnostic; codegen
// is never reached.

#include <string>
#include <string_view>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// §5.1 ("... is not implicitly convertible to the target type (§5.1)").
bool hasNotImplicitlyConvertible(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

bezel::test::OwnershipResult check(const std::string& src) {
    return parseAndCheckOwnership(src);
}

}  // namespace

// §5.1 / §27.1.1 (#2067): the direct repro — `int` literal in a `(int) -> int`
// parameter slot must be REJECTED at compile time. Before the fix this fell
// through checkAssignable's property-getter promotion (zero-arg only) and the
// Function-to-non-Function guard (which only fires in the OTHER direction),
// so the invalid call reached codegen and triggered the IR-verifier abort.
BEZEL_TEST(scalar_to_function_param_2067,
           int_literal_to_func_param_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public static method Take(f: (int) -> int): int return 0; end Take; "
        "  public static method Main(): int return Take(42); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// §5.1 (#2067): the symmetric case — `bool` in a function slot is also a
// scalar-to-function pair; must reject too. Different primitive source so a
// test that only checked `int` would miss a regression that narrowed the
// guard to `int`-only.
BEZEL_TEST(scalar_to_function_param_2067,
           bool_literal_to_func_param_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class C "
        "  public static method Take(f: (bool) -> int): int return 0; end Take; "
        "  public static method Main(): int return Take(true); end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// §5.1 (#2067): a Named (class) source into a function parameter is also a
// non-function → function pair. Even a non-record class cannot implicitly
// become a function value; the rejection must not be Primitive-specific.
BEZEL_TEST(scalar_to_function_param_2067,
           class_instance_to_func_param_rejected_spec_5_1) {
    auto r = check(
        "module M; "
        "class Box "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Box; "
        "class C "
        "  public static method Take(f: (int) -> int): int return 0; end Take; "
        "  public static method Main(): int "
        "    using var b := new Box(7); "
        "    return Take(b); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
}

// §5.1 / §27.6 (#2067): the §27.6 property-getter promotion exception is
// preserved. A zero-argument function whose return type matches the
// property's declared type still re-types the source expression as the
// function type (codegen emits a property thunk). This is the one and only
// non-function → function case the spec admits at formation sites; the fix
// must NOT reject it.
BEZEL_TEST(scalar_to_function_param_2067,
           zero_arg_property_getter_promotion_preserved_spec_27_6) {
    auto r = check(
        "module M; "
        "class Holder "
        "  public property X: int := 99; "
        "end Holder; "
        "class C "
        "  public static method Caller(f: () -> int): int return f(); "
        "  end Caller; "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    return Caller(h.X); "
        "  end Main; "
        "end C;");
    // Caller(h.X) — `h.X` is a property read; the §27.6 promotion accepts it
    // and re-types `h.X` as `() -> int`. No §5.1 error expected.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.1.1 (#2067): a function value (e.g. a bare method reference) into a
// matching function-typed parameter is still accepted. The fix added a new
// non-function → function guard; the existing Function → Function path must
// be unaffected.
BEZEL_TEST(scalar_to_function_param_2067,
           function_value_to_matching_func_param_accepted_spec_27_1_1) {
    auto r = check(
        "module M; "
        "class Id "
        "  public static method Run(x: int): int return x; end Run; "
        "end Id; "
        "class C "
        "  public static method Take(f: (int) -> int): int return 0; end Take; "
        "  public static method Main(): int return Take(Id.Run); end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
