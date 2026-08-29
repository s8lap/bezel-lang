// Spec-derived regression test for issue #3523 — a ZERO-parameter derived
// constructor never pre-populated its primary-constructor base's positional
// fields.
//
// spec §22.10 "Primary constructors" (v0.283): "A class may declare its
// primary constructor's parameters in the class header."
// spec §22.8 "Field initializers" (v0.283), construction step 0: those
// parameters are stored into their backing field slots before initializers
// run.
// spec §12.8 "Constructor chaining" (v0.283): "A derived class's constructor
// may chain to a base constructor".
//
// Nothing in any of the three makes the base's positional slots depend on the
// arity of the DERIVED constructor.
//
// WHAT THE DEFECT WAS. `Emitter::prePopulateBasePositionalFields`
// (src/codegen/LlvmBackend_Exprs2.cpp) reached its leading-`base(...)` scan
// only through
//
//     else if (cs && !cs->params.empty() && !ctorArgVals.empty() &&
//              cs->params.size() == ctorArgVals.size())
//
// where `cs` is the DERIVED class's constructor. For `class D: B` with
// `constructor()`, `cs->params` and `ctorArgVals` are both empty, so the
// whole clause was skipped and the leading `base(7)` was never scanned — the
// base's positional slot kept its calloc zero. The clause's purpose is only
// to bind the derived constructor's parameters into `paramSlots_` so that
// chain-call arguments which READ them can be lowered; a chain call whose
// arguments read nothing needs no bindings at all, so the emptiness of
// `cs->params` must not gate the scan. `Emitter::emitChainCall` does not
// cover for it either: for a base with no explicit constructor it takes the
// `!isThis && !hasAnyConstructor(targetCls)` branch, which runs the base's
// field initializers and never lowers the chain arguments.
//
// MEASURED ON `e57a1eb` (build/bezelc, real AOT compile + native run, exit 0,
// no diagnostic of any severity):
//
//   D-ctor
//   0        <-- WRONG: `base(7)` was written; 7 is expected
//   E-ctor
//   5        <-- correct
//
// `D` and `E` differ ONLY in whether the derived constructor happens to
// declare a parameter of its own; the `base(...)` argument is a constant in
// both. That pairing is what makes the row diagnostic rather than merely a
// value check.
//
// FAIL-WITHOUT / PASS-WITH: the `zero_param` rows assert `7` and fail on
// `e57a1eb` with `0`; the `one_param` control rows assert `5` and pass on
// both trees, so a fix that broke the already-working arity-matched path
// could not hide behind them.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  exit=%d stdout=[%s] diagnostics (count=%zu):\n",
                 r.exitCode, r.stdoutText.c_str(),
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

}  // namespace

// #3523 / §22.10 / §22.8 step 0 / §12.8 (v0.283): the issue's exact repro.
// `D` (zero-parameter derived constructor) and `E` (one-parameter) chain to
// the same primary-constructor base with a constant argument; both must
// populate the base's `n` slot.
BEZEL_TEST(base_positional_zero_param_derived_ctor_3523,
           zero_param_derived_ctor_prepopulates_base_positional_field) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int) "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class D: B "
        "  public constructor() "
        "    base(7); "
        "    Console.WriteLine(\"D-ctor\"); "
        "  end constructor; "
        "end D; "
        "class E: B "
        "  public constructor(v: int) "
        "    base(v); "
        "    Console.WriteLine(\"E-ctor\"); "
        "  end constructor; "
        "end E; "
        "class C public static method Main(): int "
        "  using var d := new D(); "
        "  d.Show(); "
        "  using var e := new E(5); "
        "  e.Show(); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "D-ctor\n7\nE-ctor\n5\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("D-ctor\n7\nE-ctor\n5\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3523 / §22.10 / §22.8 step 0 (v0.283): the multi-slot form. A zero-
// parameter derived constructor whose `base(...)` supplies SEVERAL constants
// must fill every one of the base's positional slots, in order — the store
// loop pairs `baseArgVals[i]` with `basePosFields[i]`, so a one-slot row
// alone could not tell a correct scan from one that stored only the first.
BEZEL_TEST(base_positional_zero_param_derived_ctor_3523,
           zero_param_derived_ctor_prepopulates_all_base_positional_fields) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, m: int) "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(m.ToString()); "
        "  end Show; "
        "end B; "
        "class D: B "
        "  public constructor() "
        "    base(7, 11); "
        "  end constructor; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); "
        "  d.Show(); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "7\n11\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n11\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3523 / §22.8 step 0 (v0.283): a base FIELD INITIALIZER that reads the
// base's primary-constructor parameter. Step 0 stores the parameters before
// step 2 runs the initializers, so the initializer must see 7, not the calloc
// zero. This is the shape #873 introduced the pre-population for, reached
// from a zero-parameter derived constructor.
BEZEL_TEST(base_positional_zero_param_derived_ctor_3523,
           zero_param_derived_ctor_base_field_initializer_reads_chain_arg) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int) "
        "  public Doubled: int := n + n; "
        "end B; "
        "class D: B "
        "  public constructor() "
        "    base(7); "
        "  end constructor; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D(); "
        "  Console.WriteLine(d.Doubled.ToString()); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "14\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("14\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3523 / §12.8 (v0.283): the control that already worked. A derived
// constructor WITH a parameter, whose `base(...)` argument READS that
// parameter — the shape the old guard existed to serve (it binds the derived
// constructor's parameters into `paramSlots_` so the chain argument can be
// lowered). A widening of the guard must not disturb it.
BEZEL_TEST(base_positional_zero_param_derived_ctor_3523,
           one_param_derived_ctor_chain_arg_reading_param_still_works) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int) "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class E: B "
        "  public constructor(v: int) "
        "    base(v + 1); "
        "  end constructor; "
        "end E; "
        "class C public static method Main(): int "
        "  using var e := new E(41); "
        "  e.Show(); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "42\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
