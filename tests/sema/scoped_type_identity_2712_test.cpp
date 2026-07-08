// Sema tests for `scoped`'s TYPE-SYSTEM rules — spec §27.2 / §22.3 / §28.14
// (v0.175, #808).
//
// §22.3: "`scoped` — on a *function-typed* parameter only" (checked against
// the RESOLVED type, so a type alias to a function type is admitted).
// §27.2 (v0.175): "`scoped` participates in function-signature identity like
// ref/out: `(scoped () -> Task) -> ()` and `(() -> Task) -> ()` are distinct
// types with no conversion between them."
//
// The full front-end (through the ownership pass) runs on each program; the
// positives assert hasErrors, the negatives assert clean.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// §22.3 (v0.175): `scoped` on a NON-function-typed parameter is rejected.
BEZEL_TEST(scoped_type_identity_2712, non_function_param_rejected_spec_22_3) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method M(scoped x: int) end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "function-typed"));
}

// §22.3 control: `scoped` on a function-typed parameter is accepted.
BEZEL_TEST(scoped_type_identity_2712, function_param_accepted_spec_22_3) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method M(scoped f: () -> ()) "
        "    f(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §22.3 (v0.175): the function-typed check is ALIAS-TRANSPARENT — a `scoped`
// parameter whose type is an alias resolving to a function type is accepted.
BEZEL_TEST(scoped_type_identity_2712, alias_to_function_type_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "type Callback := () -> (); "
        "class C "
        "  public method M(scoped f: Callback) "
        "    f(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §28.14 (v0.175): a `scoped` FIELD must be function-typed too.
BEZEL_TEST(scoped_type_identity_2712, non_function_field_rejected_spec_28_14) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _x: int; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "function-typed"));
}

// A.6 (v0.175): `scoped` on a non-function position INSIDE a function type is
// rejected (the FuncParamMods carry the same function-typed-only rule).
BEZEL_TEST(scoped_type_identity_2712, func_type_non_function_position_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _g: ((scoped [3]int) -> ())?; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "function-typed"));
}

// §27.2 (v0.175): a method with a `scoped` parameter has the scoped position
// in its FUNCTION-TYPE identity; the method reference does not convert to the
// plain-signature function type.
//
// `Take` returns `owned C` (§28.14 v0.180, #1606: a `scoped` parameter on a
// static method requires an `owned` return so a call site can resolve a
// bounding object) — the identity mismatch under test is on the PARAMETER's
// `scoped` modifier, independent of the return type.
BEZEL_TEST(scoped_type_identity_2712, scoped_to_plain_conversion_rejected_27_2) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(scoped f: () -> ()): owned C "
        "    f(); "
        "    return new C(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var h: (() -> ()) -> () := C.Take; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "identity"));
}

// §27.2 (v0.175): the reverse direction is equally rejected — a plain-
// signature method reference does not convert to a scoped-position type.
BEZEL_TEST(scoped_type_identity_2712, plain_to_scoped_conversion_rejected_27_2) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method TakePlain(f: () -> ()) "
        "    f(); "
        "  end TakePlain; "
        "  public static method Main(): int "
        "    var g: (scoped () -> ()) -> () := C.TakePlain; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "identity"));
}

// §27.2 control: the MATCHING scoped-position assignment is accepted — the
// identity rule forbids conversion, not the type itself.
//
// `Take` returns `owned C` for the same §28.14 v0.180 (#1606) reason as
// above.
BEZEL_TEST(scoped_type_identity_2712, matching_scoped_signature_accepted_27_2) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Take(scoped f: () -> ()): owned C "
        "    f(); "
        "    return new C(); "
        "  end Take; "
        "  public static method Main(): int "
        "    var g: (scoped () -> ()) -> () := C.Take; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
