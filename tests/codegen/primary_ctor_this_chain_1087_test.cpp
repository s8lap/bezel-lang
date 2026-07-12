// Regression tests for #1087 (§22.8): a primary-constructor class constructed
// via a `this(...)`-chaining additional constructor ran NONE of the primary
// ctor's construction sequence — the positional params and field initializers
// (and base chain) all stayed calloc-zero. The additional ctor body skips
// emitOwnFieldInit for a `this(...)` delegation (expecting the chained ctor to
// run it), but emitChainCall had no branch for `this(...)` into a primary-ctor
// class (whose primary ctor has no explicit ConstructorSymbol), so the chain
// was a silent no-op. The fix reproduces the primary ctor's construction in
// emitChainCall: store the chained positional args into the param-fields, run
// the base chain, then the field initializers — mirroring `new C(args)`.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

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

// #1087: a field initializer runs when the primary ctor is reached via this().
BEZEL_TEST(primary_ctor_this_chain_1087, field_initializer_runs) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C(x: int) "
        "  public _f: int := 42; "
        "  public constructor() this(99); end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(); return c._f; "      // 42, was 0
        "  end Main; end C;"), 42);
}

// The chained positional argument (x := 99) is stored into the param-field.
BEZEL_TEST(primary_ctor_this_chain_1087, positional_arg_stored) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C(x: int) "
        "  public _f: int := 42; "
        "  public method GetX(): int => x; "
        "  public constructor() this(99); end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.GetX() + c._f; "  // 99 + 42 = 141
        "  end Main; end C;"), 141);
}

// The base chain runs too: a base field initializer is observed.
BEZEL_TEST(primary_ctor_this_chain_1087, base_chain_runs) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Base public _b: int := 10; end Base; "
        "class C(x: int): Base "
        "  public _f: int := 42; "
        "  public method GetX(): int => x; "
        "  public constructor() this(99); end constructor; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.GetX() + c._f + c._b; "  // 151
        "  end Main; end C;"), 151);
}

// Constructing the primary ctor DIRECTLY (`new C(5)`) is unaffected.
BEZEL_TEST(primary_ctor_this_chain_1087, direct_primary_ctor_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C(x: int) "
        "  public _f: int := 42; "
        "  public method GetX(): int => x; "
        "  public static method Main(): int "
        "    using var c := new C(5); return c.GetX() + c._f; "  // 5 + 42 = 47
        "  end Main; end C;"), 47);
}
