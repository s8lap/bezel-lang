// Regression tests for #3792 (§22.5 / §22.8 step 1 / §12.8, v0.287):
// the IMPLICIT base chain — a derived constructor that writes NO
// `base(...)` clause at all — selected its target by EXACT arity
// (`ctorByArity(b, 0, nullptr)` at LlvmBackend_Setup.cpp:13169). A
// derived class whose base constructor parameters are ALL §22.5-
// defaulted therefore found no arity-0 candidate, fell through to the
// `else if (!emitImportedBaseFieldInit(...))` arm, the base had no
// positional fields for that arm to pre-populate, and the base subobject
// was NEVER constructed. The program compiled clean at every severity
// and printed calloc-zero — a quiet-wrong-answer outcome no oracle
// keyed on exit status or a diagnostic can see, which is why every row
// below asserts printed VALUES.
//
// `new` sites had honoured §22.5's "value an omitted optional parameter
// receives IS its declared default" since #3082 (ctorForArgs admits an
// omitted defaulted tail). `emitChainCall` and the §22.10 inheritance
// clause gained it in #3777. The IMPLICIT base chain is the last chain
// site; fixing it is widening the same single resolver (`ctorForArgs`)
// to it and materializing the §22.5 defaults through the SAME
// `fillOmittedCtorArgs` helper the other chain sites already use, so
// "what value does an omitted parameter take" stays single-sourced
// across every chain site (CLAUDE.md §6).

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

// Row 1: issue repro — derived class with ALL-§22.5-defaulted base
// ctor, no explicit `base(...)` clause. On 48fe111 this printed `0`
// (calloc zero); the fix prints `5`.
BEZEL_TEST(implicit_base_arity_3792,
           implicit_base_to_all_defaulted_ctor_runs_base) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  private _a: int; "
        "  public constructor(a: int := 5) "
        "    _a := a; "
        "  end constructor; "
        "  public method A(): int => _a; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() "
        "    var dummy := 0; "
        "  end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    Console.WriteLine(d.A().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.stdoutText != "5\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 2: control — derived class with EXPLICIT `base(7)`. Must print
// `7` (no regression). This is the shape #3777's `base_clause_*`
// regression rows already pin; adding the row here pins it again in
// the implicit-base grid so a future widening of `ctorForArgs`'s
// admission cannot silently route a non-zero base clause through the
// implicit path.
BEZEL_TEST(implicit_base_arity_3792,
           explicit_base_clause_zero_param_base_unchanged) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  private _a: int; "
        "  public constructor(a: int := 5) "
        "    _a := a; "
        "  end constructor; "
        "  public method A(): int => _a; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() "
        "    base(7); "
        "  end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    Console.WriteLine(d.A().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.stdoutText != "7\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 3: control — derived class with EXPLICIT `base()` against an
// arity-0 base ctor (the pre-existing parameterless path that
// `ctorByArity(b, 0, nullptr)` correctly matched on 48fe111). Must
// keep printing `42` — a fix that broke the exact-arity arm in
// `ctorForArgs` would surface here.
BEZEL_TEST(implicit_base_arity_3792,
           explicit_base_clause_against_explicit_arity_zero_ctor) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  private _a: int; "
        "  public constructor() "
        "    _a := 42; "
        "  end constructor; "
        "  public method A(): int => _a; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() "
        "    base(); "
        "  end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    Console.WriteLine(d.A().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.stdoutText != "42\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 4: control — derived class with explicit `base()` against a
// MIXED defaulted + non-defaulted base ctor. The arity-0 ctor exists
// and is selected (§22.6's exact-arity preference wins over the
// all-defaulted one — `ctorForArgs`'s order is §22.6's order). A fix
// that admitted the all-defaulted ctor BEFORE the arity-0 one would
// print `5` here instead of `99`.
BEZEL_TEST(implicit_base_arity_3792,
           mixed_base_ctors_explicit_zero_prefers_arity_zero) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  private _a: int; "
        "  public constructor(a: int := 5) "
        "    _a := a; "
        "  end constructor; "
        "  public constructor() "
        "    _a := 99; "
        "  end constructor; "
        "  public method A(): int => _a; "
        "end Base; "
        "class Derived : Base "
        "  public constructor() "
        "    base(); "
        "  end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    using var d := new Derived(); "
        "    Console.WriteLine(d.A().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.stdoutText != "99\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("99\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 5: control — multi-level chain. `Derived2 extends Derived`,
// `Derived extends Base` with `Base`'s ctor all-defaulted. The
// implicit base chain at `Derived2`'s constructor must recurse: it
// calls `Derived`'s ctor, which calls `Base`'s all-defaulted ctor with
// the §22.5 defaults materialized at each level. Prints `5/7`.
BEZEL_TEST(implicit_base_arity_3792,
           multi_level_implicit_chain_recurses_through_all_defaulted_base) {
    JitResult r = runAot(
        "module M; "
        "class Base "
        "  private _a: int; "
        "  public constructor(a: int := 5) "
        "    _a := a; "
        "  end constructor; "
        "  public method A(): int => _a; "
        "end Base; "
        "class Mid : Base "
        "  private _b: int; "
        "  public constructor(b: int := 7) "
        "    _b := b; "
        "  end constructor; "
        "  public method B(): int => _b; "
        "end Mid; "
        "class Top : Mid "
        "  public constructor() "
        "    var dummy := 0; "
        "  end constructor; "
        "end Top; "
        "class C "
        "  public static method Main(): int "
        "    using var t := new Top(); "
        "    Console.WriteLine(t.A().ToString()); "
        "    Console.WriteLine(t.B().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.stdoutText != "5\n7\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n7\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}