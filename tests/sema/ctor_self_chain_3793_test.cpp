// §22.10 / §12.8 / §22.8 (issue #3793, v0.287): a constructor's leading
// `this(...)` chain call must not resolve to the constructor that contains
// it. §22.10 ("Primary constructors" / "Constructors") states that
// `this(...)` "chains to another constructor in the same class" — "another"
// excludes self; §12.8 gives the same rule from the chain-call side.
//
// WHAT THE DEFECT WAS. Sema accepted a self-chain silently; codegen's
// `ctorByArity` / `ctorForArgs` already takes an `exclude` carve-out for
// `cs` to prevent infinite recursion (LlvmBackend_impl.hpp:12259 and
// fix #3777's widening), but the SEMANTIC omission was silent: the call
// compiled clean, codegen emitted nothing for a non-primary-ctor class
// (`class B` with no §22.10 primary, no sibling ctor), and the object
// stayed calloc-zero. The first field read then returned 0 / null / etc.
// — silently producing a wrong value with no diagnostic at any severity.
// Measured on r12-cycle-1 (d6d7d17):
//
//   class B
//     private _a: int;
//     public constructor(a: int)
//       this(a);              // self-chain
//     end constructor;
//     public method A(): int return this._a; end A;
//   end B;
//   ...
//   var b := new B(7);
//   Console.WriteLine(b.A().ToString());    // prints "0", no error
//
// WHAT THE FIX DOES. `Resolver::checkTypeConstructorChains`
// (TypeResolution_Checks.cpp) now emits a §22.10 error when the leading
// `this(...)` chain call's resolution by arity, against the class's
// non-static ConstructorSymbols, would land on `cs` itself — i.e. the
// chain call's arity matches `cs->params.size()` and NO OTHER non-static
// ConstructorSymbol of the class has that arity. Apply ONLY to classes
// without a primary constructor: when a primary exists (`hasPositional
// Fields`), the §22.10 primary arm in `emitChainCall`
// (LlvmBackend_Setup.cpp:12189, fix #1087) routes the call to the
// primary — which carries no ConstructorSymbol — rather than `cs`, so
// a self-arithmetic match is never actually reached and would be a
// false rejection.
// Row 1 (rejects_self_chain): the issue's repro — `constructor(a)
// this(a);` against an empty class — must produce the §22.10 diagnostic.
// Row 2 (accepts_chain_to_sibling): valid `this(0)` chain to a
// DIFFERENT constructor — must compile clean.
// Row 3 (accepts_base_chain): valid `base()` chain from a derived class
// — must compile clean.
// Row 4 (accepts_multi_level_chain): multi-level chain
// (`constructor(a) this(a, 0); constructor(a, b) this();` — chain
// targets ANOTHER ctor, not itself) — must compile clean.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff SOME diagnostic contains `sub`. Rejection rows assert on
// SPECIFIC message text, never on the coarse `hasErrors()` — `runAot`
// synthesizes an Error diagnostic for a runtime SIGSEGV/ICE too, so
// `hasErrors()` cannot tell a genuine reject from the very crash this
// fix removes.
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

// The §22.10 self-chain diagnostic's two stable fragments. Pin both
// separately: the "cannot chain to its own constructor" phrase names
// the rule, and the "§22.10" citation pins the section so a wording
// tweak that rephrases the sentence does not silently pass.
constexpr const char* kSelfChainDiag =
    "a constructor cannot chain to its own constructor";
constexpr const char* kSection22_10 = "§22.10";

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — REJECT: the issue's repro. A single `constructor(a)` whose body
// chains via `this(a)` has no sibling ctor and no primary ctor, so the
// chain call's only resolution is the constructor itself. Sema MUST
// reject with the §22.10 self-chain diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_self_chain_3793, rejects_direct_self_chain_spec_22_10) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  public constructor(a: int) "
        "    this(a); "
        "  end constructor; "
        "  public method A(): int return this._a; end A; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(7); "
        "    Console.WriteLine(b.A().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors() == false ||
        !hasError(r, kSelfChainDiag) ||
        !hasError(r, kSection22_10)) {
        dumpDiagnostics(r);
    }
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasError(r, kSelfChainDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kSection22_10));
    // Belt-and-braces: the program MUST NOT have run. A run-and-print
    // would be the very silent wrong-value this fix removes.
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 2 — ACCEPT: a valid `this(0)` chain to a DIFFERENT constructor
// (the parameterless one). `cs` itself has one parameter; the chain
// call has zero args; a sibling constructor with zero parameters
// exists, so the chain resolves to the sibling, NOT to `cs`. Sema MUST
// accept — a "this(...) chain to itself" diagnostic here would be a
// false rejection.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_self_chain_3793, accepts_chain_to_sibling_spec_22_10) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  public constructor() "
        "    _a := 0; "
        "  end constructor; "
        "  public constructor(a: int) "
        "    this(); "
        "    _a := a; "
        "  end constructor; "
        "  public method A(): int return this._a; end A; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(7); "
        "    Console.WriteLine(b.A().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 3 — ACCEPT: a valid `base()` chain from a derived class.
// `base(...)` is NOT a `this(...)` chain and is therefore outside this
// fix's scope; the §12.8 chain-call placement rule is the §12.8 check,
// not the §22.10 self-chain check. A class with an explicit base
// constructor and a derived class that chains to it must compile.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_self_chain_3793, accepts_base_chain_spec_22_10) {
    auto r = runAot(
        "module M; "
        "class Base "
        "  public constructor() "
        "  end constructor; "
        "end Base; "
        "class Derived "
        "    : Base "
        "  public constructor() "
        "    base(); "
        "  end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var d := new Derived(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 4 — ACCEPT: a multi-level chain that targets ANOTHER constructor
// of the same class, not itself. `constructor(a) this(a, 0);` resolves
// to the (a, b) sibling; `constructor(a, b) this();` resolves to the
// () sibling. Neither chain's resolution is the calling ctor, so
// neither is a self-chain — and this row pins that the false-rejection
// guard covers both the (chain-arities-match-sibling-arity) and the
// (sibling-of-different-arity) shapes.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_self_chain_3793, accepts_multi_level_chain_spec_22_10) {
    auto r = runAot(
        "module M; "
        "class B "
        "  private _a: int; "
        "  private _b: int; "
        "  public constructor() "
        "    _a := 0; "
        "    _b := 0; "
        "  end constructor; "
        "  public constructor(a: int, b: int) "
        "    this(); "
        "    _a := a; "
        "    _b := b; "
        "  end constructor; "
        "  public constructor(a: int) "
        "    this(a, 0); "
        "  end constructor; "
        "  public method A(): int return this._a + this._b; end A; "
        "end B; "
        "class C "
        "  public static method Main(): int "
        "    var b := new B(7); "
        "    Console.WriteLine(b.A().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
}