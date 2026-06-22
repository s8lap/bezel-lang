// Spec-derived tests for issue #967 — `ref`/`out` parameters on a CONSTRUCTOR
// (§22.3).
//
// §22.3 describes `ref`/`out` as general parameter modifiers with no method-only
// restriction, so they are valid on constructor parameters. Before the fix, the
// constructor's LLVM signature lowered every parameter to its value type — it
// did not apply the `isByRefMod ? ptrTy()` rule the method path uses — so a
// `ref` constructor parameter was declared as a value while `lowerNew` passed
// the storage address and the body read through it as a reference: malformed IR
// ("Both operands to a binary operator are not of the same type!"), for BOTH the
// correct `new C(ref x)` and the (separately-wrong) `new C(x)`.
//
// THE FIX lowers a `ref`/`out` constructor parameter to a pointer in the ctor
// signature (mirroring the method path) and enforces the mandatory call-site
// keyword for the single bound constructor (mirroring #952 for methods).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// A `ref` constructor parameter mutates the caller's variable (was malformed
// IR). x starts 5; the ctor does n := n + 1 → x == 6.
BEZEL_TEST(ctor_ref_param_967, ref_ctor_param_mutates) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public V: int; "
        "  public constructor(ref n: int) n := n + 1; V := n; end constructor; "
        "  public static method Main(): int "
        "    var x := 5; "
        "    var c := new C(ref x); "
        "    c.Dispose(); "
        "    return x; "  // 6
        "  end Main; "
        "end C;"), 6);
}

// A mixed value + `ref` parameter list. y starts 10; ctor does b := b + a (a=5)
// → y == 15.
BEZEL_TEST(ctor_ref_param_967, mixed_value_and_ref_params) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public V: int; "
        "  public constructor(a: int, ref b: int) b := b + a; V := b; end constructor; "
        "  public static method Main(): int "
        "    var y := 10; "
        "    var c := new C(5, ref y); "
        "    c.Dispose(); "
        "    return y; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// §22.3 (#967): omitting the mandatory `ref` for a `ref` constructor parameter
// is a compile error — was a malformed-IR/silent-mutation before.
BEZEL_TEST(ctor_ref_param_967, missing_ref_keyword_rejected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public V: int; "
        "  public constructor(ref n: int) n := n + 1; V := n; end constructor; "
        "  public static method Main(): int "
        "    var x := 5; "
        "    var c := new C(x); "  // missing `ref` → compile error
        "    c.Dispose(); "
        "    return x; "
        "  end Main; "
        "end C;"), -999);
}

// §22.3 (#967): a superfluous `ref` for a plain constructor parameter is a
// compile error.
BEZEL_TEST(ctor_ref_param_967, superfluous_ref_keyword_rejected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public V: int; "
        "  public constructor(n: int) V := n; end constructor; "
        "  public static method Main(): int "
        "    var x := 9; "
        "    var c := new C(ref x); "  // superfluous `ref` → compile error
        "    c.Dispose(); "
        "    return x; "
        "  end Main; "
        "end C;"), -999);
}

// No over-rejection: an ordinary (no-modifier) constructor call is unaffected.
BEZEL_TEST(ctor_ref_param_967, plain_ctor_call_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public V: int; "
        "  public constructor(n: int) V := n; end constructor; "
        "  public static method Main(): int "
        "    var c := new C(42); "
        "    var r := c.V; "
        "    c.Dispose(); "
        "    return r; "  // 42
        "  end Main; "
        "end C;"), 42);
}
