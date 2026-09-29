// §12.8 / §22.5 / §22.6 / §22.10 (issue #4791 sema half, v0.307; design per
// #4854): a leading `this(...)` chain call with a NAMED argument that names no
// parameter of the containing constructor.
//
// WHAT THE DEFECT WAS. The #3793 self-chain check
// (`Resolver::checkTypeConstructorChains`, TypeResolution_Checks.cpp) runs
// before any argument is typed and judges a `this(...)` chain by ARITY alone:
// when its argument count matches the containing constructor and no other
// constructor, it reports "chains to the constructor itself". For
//
//   public constructor(x: int := 1, y: int := 0, z: int := 0) ...
//   public constructor(s: string, y: int)
//     this(y := 42, x := 1);
//
// that is a false rejection: `x` names no parameter of `(s, y)`, so §22.5
// binding cannot bind the call to it, and it binds the three-parameter
// constructor ("chains to another constructor in the same type", §12.8).
//
// WHAT THE FIX DOES. The resolver DEFERS exactly those calls
// (`deferredNamedThisChain`, SymbolCollection.cpp — the one predicate both
// passes read) to `ExprTC::checkDeferredNamedThisChain`, which decides them
// with the argument types in hand: accept only if exactly one OTHER
// constructor binds the call by name, that one is §22.6-applicable, and no
// chain path from it leads back. The deferred domain is exactly what the
// resolver used to reject, so every other outcome is a reject — with a
// SPECIFIC diagnostic, never the old silent-accept of the rejected first
// attempt (2d3dac0c), which skipped the check for ANY non-self-naming named
// argument and so accepted rows 1-7 below (unbound name, wrong type, extra
// name, no sibling at all, a PARENT's parameter name, positional-after-named,
// and a named mutual recursion).
//
// Rows 1-7 are the #4791 review's probes; each asserts its specific
// diagnostic and that the program did not run. Row 8 is the #4854-flagged
// restriction (more than one name-binding sibling is rejected, not ranked).
// Row 9 pins that a named argument naming the containing constructor's OWN
// parameter is NOT deferred (still the #3793 diagnostic). Rows 10-11 are the
// accepts, asserting the constructed VALUE.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

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

// The three #4791 diagnostics, each pinned by a phrase unique to it.
constexpr const char* kBindsNone =
    "binds no other constructor of `K`: no other constructor accepts these "
    "arguments by parameter name and type, and a constructor cannot chain to "
    "its own constructor (§12.8, §22.5, §22.6)";
constexpr const char* kBindsNoneChild =
    "binds no other constructor of `Child`: no other constructor accepts "
    "these arguments by parameter name and type";
constexpr const char* kBindsMany =
    "binds more than one other constructor of `K` by parameter name";
constexpr const char* kChainsBack =
    "chains back to the constructor itself through another constructor; a "
    "constructor cannot chain to its own constructor (§22.10)";
// The #3793 resolver diagnostic.
constexpr const char* kSelfChain3793 =
    "chains to the constructor itself; a constructor cannot chain to its own "
    "constructor (§22.10)";

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

// A K with the three-parameter all-defaulted target and a `(s, y)`
// constructor whose chain call is `chain`.
std::string xyzProgram(const char* chain) {
    return std::string(
               "module M; "
               "class K "
               "  public Tag: string; "
               "  public constructor(x: int := 1, y: int := 0, z: int := 0) "
               "    Tag := $\"XY{x},{y},{z}\"; "
               "  end constructor; "
               "  public constructor(s: string, y: int) ") +
           chain +
           "  end constructor; "
           "end K; "
           "class C "
           "  public static method Main(): int "
           "    var k := new K(\"hello\", 3); "
           "    Console.WriteLine(k.Tag); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// Row 1 (c_noname): `zz` names no parameter of any constructor.
BEZEL_TEST(ctor_chain_self_reference_4791, rejects_unbound_name_spec_22_5) {
    auto r = runAot(
        "module M; "
        "class K "
        "  private _s: string; "
        "  private _n: int; "
        "  public constructor(s: string) "
        "    this(zz := 1); "
        "    _s := s; "
        "  end constructor; "
        "  public constructor(a: int, b: int) _n := a + b; _s := \"ab\"; "
        "  end constructor; "
        "  public method S(): string return $\"{_s}/{_n}\"; end S; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(\"hello\"); "
        "    Console.WriteLine(k.S()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRejected(r, kBindsNone);
}

// Row 2 (d_wrongtype): binds `(x, y, z)` by name, but `"str"` is not
// §22.6-applicable to `x: int`.
BEZEL_TEST(ctor_chain_self_reference_4791, rejects_inapplicable_type_spec_22_6) {
    auto r = runAot(xyzProgram("this(x := \"str\", y := 1); "));
    expectRejected(r, kBindsNone);
}

// Row 3 (g_extra_named): `w` names no parameter of `(x, y, z)`.
BEZEL_TEST(ctor_chain_self_reference_4791, rejects_extra_unbound_name_spec_22_5) {
    auto r = runAot(xyzProgram("this(y := 42, w := 1); "));
    expectRejected(r, kBindsNone);
}

// Row 4 (f_selfnamed): the only constructor; `zz` is not one of its names.
BEZEL_TEST(ctor_chain_self_reference_4791, rejects_no_other_constructor_spec_12_8) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public Tag: string; "
        "  public constructor(a: int, b: int) "
        "    this(b := 1, zz := 2); "
        "    Tag := \"k\"; "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(1, 2); "
        "    Console.WriteLine(k.Tag); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRejected(r, kBindsNone);
}

// Row 5 (s1): `y` names a PARENT constructor's parameter; `this(...)` chains
// within the same type only (§12.8), so nothing binds.
BEZEL_TEST(ctor_chain_self_reference_4791, rejects_parent_param_name_spec_12_8) {
    auto r = runAot(
        "module M; "
        "class Parent "
        "  public readonly property P: int; "
        "  public constructor(y: int) P := y; end constructor; "
        "end Parent; "
        "class Child : Parent "
        "  private _s: string; "
        "  public constructor(s: string) "
        "    this(y := 42); "
        "    _s := s; "
        "  end constructor; "
        "  public method S(): string return $\"{_s}/{P}\"; end S; "
        "end Child; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Child(\"hello\"); "
        "    Console.WriteLine(c.S()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRejected(r, kBindsNoneChild);
}

// Row 6 (s5): a positional argument after a named one binds nothing (§22.5).
BEZEL_TEST(ctor_chain_self_reference_4791,
           rejects_positional_after_named_spec_22_5) {
    auto r = runAot(
        "module M; "
        "class Parent "
        "  public readonly property P: int; "
        "  public constructor(x: int, s: string) P := x; end constructor; "
        "end Parent; "
        "class Child : Parent "
        "  private _s: string; "
        "  private _y: int; "
        "  public constructor(s: string, y: int) "
        "    this(x := 99, s); "
        "    _s := s; "
        "  end constructor; "
        "  public method S(): string return $\"{_s}/{P}\"; end S; "
        "end Child; "
        "class C "
        "  public static method Main(): int "
        "    var c := new Child(\"hello\", 1); "
        "    Console.WriteLine(c.S()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRejected(r, kBindsNoneChild);
}

// Row 7 (e_cycle): `(s)` chains to `(x, y)` by name, which chains straight
// back to `(s)` by name — an unbounded recursion at run time.
BEZEL_TEST(ctor_chain_self_reference_4791, rejects_named_mutual_chain_spec_22_10) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public Tag: string; "
        "  public constructor(s: string) "
        "    this(x := 1); "
        "    Tag := s; "
        "  end constructor; "
        "  public constructor(x: int, y: int := 0) "
        "    this(s := \"a\"); "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(\"hello\"); "
        "    Console.WriteLine(k.Tag); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRejected(r, kChainsBack);
}

// Row 8: two siblings bind `this(x := 1)` by name. Rejected rather than
// ranked — a restriction of this narrow, since codegen's `ctorForArgs` keys
// on names, not the §22.6 ranking; the general resolution is #4854.
BEZEL_TEST(ctor_chain_self_reference_4791,
           rejects_two_name_binding_siblings_restriction_4854) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public Tag: string; "
        "  public constructor(x: int, y: int := 0) Tag := \"xy\"; "
        "  end constructor; "
        "  public constructor(x: int, z: int := 0, w: int := 0) Tag := \"xzw\"; "
        "  end constructor; "
        "  public constructor(s: string) "
        "    this(x := 1); "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(\"hello\").Tag); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRejected(r, kBindsMany);
}

// Row 9: a named argument naming the containing constructor's OWN
// parameter is not deferred; the #3793 self-chain diagnostic still fires.
BEZEL_TEST(ctor_chain_self_reference_4791,
           own_param_name_still_self_chain_3793_spec_22_10) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public Tag: string; "
        "  public constructor(a: int) "
        "    this(a := 1); "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(2).Tag); "
        "    return 0; "
        "  end Main; "
        "end C;");
    expectRejected(r, kSelfChain3793);
}

// Row 10 (h_valid): the issue's accept — binds `(x, y, z)` by name; `z`
// takes its default.
BEZEL_TEST(ctor_chain_self_reference_4791, accepts_named_chain_to_sibling_spec_22_5) {
    auto r = runAot(xyzProgram("this(y := 42, x := 1); "));
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "XY1,42,0\n");
}

// Row 11: the chosen sibling itself chains onward (to `()`), with no path
// back — accepted, and the chain runs in order.
BEZEL_TEST(ctor_chain_self_reference_4791,
           accepts_named_chain_through_acyclic_sibling_spec_12_8) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public Tag: string; "
        "  public constructor() Tag := \"0\"; end constructor; "
        "  public constructor(x: int, y: int := 5) "
        "    this(); "
        "    Tag := $\"{Tag}-{x},{y}\"; "
        "  end constructor; "
        "  public constructor(s: string) "
        "    this(x := 7); "
        "    Tag := $\"{Tag}-{s}\"; "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(new K(\"hi\").Tag); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0-7,5-hi\n");
}
