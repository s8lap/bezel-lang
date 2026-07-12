// Codegen fix #582 — static readonly field initializers that reference other
// static fields of the same or a different type must evaluate in declaration
// order at runtime (§22.13).
//
// §22.13: "Within a single type, static fields are initialized in textual
// declaration order."  A non-literal initializer expression (e.g. `B := A * 2`)
// must be evaluated at runtime after all earlier fields in the same type have
// been initialized.  The static constructor body, if any, runs after ALL field
// initializers complete.
//
// Root cause before the fix: `constInitFor` only handles compile-time-constant
// initializers (int/bool/float/string literals); a non-constant initializer
// fell through to zero.  No runtime evaluation path existed.
//
// The fix synthesizes a `bz$<T>$sfinit` `void()` function for each type that
// has at least one non-constant static field initializer and injects it into
// the per-type static initialisation sequence (field-init runs first, then the
// explicit static constructor body if one is present).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §22.13 example 1 (issue #582): B := A * 2 must see A == 10, so B == 20.
// Before the fix B read as 0.
BEZEL_TEST(codegen_582, static_field_init_references_prior_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly A: int := 10; "
        "  public static readonly B: int := A * 2; "
        "  public static method Main(): int return B; end Main; "
        "end C;"), 20);
}

// §22.13: A alone is still correct (literal init, unaffected).
BEZEL_TEST(codegen_582, static_field_literal_init_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly A: int := 10; "
        "  public static method Main(): int return A; end Main; "
        "end C;"), 10);
}

// §22.13: chain A → B → C, each referencing its predecessor.
BEZEL_TEST(codegen_582, static_field_init_chain) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly A: int := 3; "
        "  public static readonly B: int := A + 1; "
        "  public static readonly CC: int := B * 2; "
        "  public static method Main(): int return CC; end Main; "
        "end C;"), 8);
}

// §22.13: independent static fields must each initialize correctly and remain
// independent of one another.
BEZEL_TEST(codegen_582, static_field_init_independent_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly X: int := 5; "
        "  public static readonly Y: int := 7; "
        "  public static method Main(): int return X + Y; end Main; "
        "end C;"), 12);
}

// §22.13: cross-class reference.  D.Y reads C.X, which must already be 10.
BEZEL_TEST(codegen_582, static_field_init_cross_class) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static readonly X: int := 10; end C; "
        "class D "
        "  public static readonly Y: int := C.X + 5; "
        "  public static method Main(): int return D.Y; end Main; "
        "end D;"), 15);
}

// §22.13: field initializer and static constructor coexist — field inits run
// first, the static ctor body runs after all field initializers have completed.
BEZEL_TEST(codegen_582, static_field_init_then_static_ctor_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static readonly A: int := 4; "
        "  public static B: int := A * 3; "       // non-const, set to 12
        "  public static constructor() "
        "    B := B + 1; "                         // increments to 13
        "  end constructor; "
        "  public static method Main(): int return B; end Main; "
        "end C;"), 13);
}

// §22.13 regression: a static constructor alone (no non-const field inits)
// must still work correctly — the synthesized path must not interfere.
BEZEL_TEST(codegen_582, static_ctor_only_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public static V: int := 0; "
        "  public static constructor() V := 99; end constructor; "
        "end N; "
        "class C public static method Main(): int return N.V; end Main; "
        "end C;"), 99);
}

// §22.13 regression: literal-only field initializers (no non-const, no ctor)
// must be unchanged.
BEZEL_TEST(codegen_582, literal_only_static_fields_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class N "
        "  public static readonly A: int := 10; "
        "  public static readonly B: int := 20; "
        "end N; "
        "class C public static method Main(): int return N.A + N.B; end Main; "
        "end C;"), 30);
}
