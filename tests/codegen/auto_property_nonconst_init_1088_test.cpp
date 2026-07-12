// Regression tests for #1088 (§23.1 / §22.13): an auto-property initializer with
// a NON-CONSTANT, name-resolving value (a static method call or static-field
// reference) was silently dropped — the backing field stayed calloc-zero.
// Sema visits the auto-property initializer, but (unlike a FIELD initializer,
// fixed by #582) it did not make moduleScope_ reachable as currentScope_, so a
// class-qualified name (`C.Default()` / `C.K`) resolved to null → no exprSymbol
// → codegen lowerExpr returned null → the init store was skipped. A *foldable*
// initializer (`1 + 1`) needs no name resolution and worked. The fix mirrors
// #582 for auto-property initializers.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); test values ≤ 255.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1088: an auto-property initialized from a static METHOD CALL holds the value.
BEZEL_TEST(auto_property_nonconst_init_1088, static_method_call_init) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Default(): int => 42; "
        "  public property P: int := C.Default(); "
        "  public static method Main(): int "
        "    using var c := new C(); return c.P; "       // 42, was 0
        "  end Main; end C;"), 42);
}

// An auto-property initialized from a static FIELD reference.
BEZEL_TEST(auto_property_nonconst_init_1088, static_field_ref_init) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static K: int := 7; "
        "  public property P: int := C.K; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.P; "       // 7, was 0
        "  end Main; end C;"), 7);
}

// The non-constant init also works when the class has an explicit constructor
// (the field-init runs through emitOwnFieldInit on the ctor path).
BEZEL_TEST(auto_property_nonconst_init_1088, with_explicit_constructor) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Default(): int => 42; "
        "  public property P: int := C.Default(); "
        "  public constructor() end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.P; "       // 42, was 0
        "  end Main; end C;"), 42);
}

// A foldable constant initializer still works (regression guard — it never went
// through the broken name-resolution path).
BEZEL_TEST(auto_property_nonconst_init_1088, foldable_const_init_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property P: int := 1 + 1; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.P; "       // 2
        "  end Main; end C;"), 2);
}

// A plain literal auto-property initializer is unchanged.
BEZEL_TEST(auto_property_nonconst_init_1088, literal_init_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property P: int := 42; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.P; "       // 42
        "  end Main; end C;"), 42);
}
