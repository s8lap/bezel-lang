// §12.8 / §22.5 / §22.6 / §5.1 (issue #4854, v0.307): a `this(...)` /
// `base(...)` constructor chain call must be APPLICABLE to a constructor of
// its target — the same §22.5 binding and §22.6 applicability a `new T(...)`
// gets. §12.8: "`this(...)` chains to another constructor in the same type";
// §22.6 stage 3 is "applicability over the (substituted) signatures: parameter
// types under the implicit conversions (§5.1)", and stage 8 "failing a unique
// winner: the ambiguity error (or, with no applicable candidate at all, the
// no-candidate error)".
//
// WHAT THE DEFECT WAS. #4824 (`checkChainCallBinding`) rejects a chain call no
// candidate §22.5-BINDS, and #4791 decides the named `this(...)` chains the
// resolver defers. Nothing checked the TYPES of a call that some explicit
// constructor binds. MEASURED on 028fcb34: `base("x")` into
// `Parent.constructor(y: int)`, `base(y := "x")`, `this(s, 2)` into
// `(x: int := 1, y: int := 0)`, `base(true)` against `(y: int)` / `(y:
// string)` all compiled clean and ran — codegen's `ctorForArgs` found no
// type-compatible target, skipped the chain, and the object stayed calloc-zero
// (`hello/0`).
//
// WHAT THE FIX DOES. `ExprTC::checkChainCallApplicability` (called from
// `checkChainCallBinding` once some explicit constructor binds) runs the
// shared `new` helpers: `checkArgsAgainstParams` for a lone binding
// candidate, `resolveOverload` for several.
//
// Rows 1-3 are the issue's s1 / s5 / `this(1, 2)` repros (rejected since
// #4824/#4791 — pinned here so the issue's own shapes stay rejected). Row 4 is
// the issue's `base(z := 1)`. Rows 5-10 are the type-applicability gap this
// fix closes and FAIL on 028fcb34. Rows 11-15 are accept controls asserting
// the constructed VALUE, including the issue's valid shape (`XY1,4`).

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

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

void expectRan(const JitResult& r, const char* out) {
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

// The issue's Parent / Child shape: `parentCtors` inside `Parent`, and a
// `Child(s: string)` constructor whose chaining position holds `chain`.
std::string childProgram(const char* parentCtors, const char* chain,
                         const char* extraChildCtors = "") {
    return std::string(
               "module M; "
               "class Parent "
               "  public readonly property P: int; ") +
           parentCtors +
           "end Parent; "
           "class Child : Parent "
           "  private _s: string; "
           "  public constructor(s: string) " +
           chain +
           "    _s := s; "
           "  end constructor; " +
           extraChildCtors +
           "  public method S(): string return $\"{_s}/{P}\"; end S; "
           "end Child; "
           "class C "
           "  public static method Main(): int "
           "    var c := new Child(\"hello\"); "
           "    Console.WriteLine(c.S()); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

constexpr const char* kParentY =
    "  public constructor(y: int) P := y; end constructor; ";
constexpr const char* kParentIntOrString =
    "  public constructor(y: int) P := y; end constructor; "
    "  public constructor(y: string) P := y.Length; end constructor; ";

// A K with a two-parameter all-defaulted target and a `(s: string)`
// constructor whose chaining position holds `chain` (the issue's valid shape).
std::string kProgram(const char* chain) {
    return std::string(
               "module M; "
               "class K "
               "  public Tag: string; "
               "  public constructor(x: int := 1, y: int := 0) "
               "    Tag := $\"XY{x},{y}\"; "
               "  end constructor; "
               "  public constructor(s: string) ") +
           chain +
           "  end constructor; "
           "end K; "
           "class C "
           "  public static method Main(): int "
           "    Console.WriteLine(new K(\"abcd\").Tag); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Two overloads of K plus a `(s: string)` constructor chaining with `chain`.
std::string overloadProgram(const char* ctors, const char* chain) {
    return std::string(
               "module M; "
               "class K "
               "  public Tag: string; ") +
           ctors +
           "  public constructor(s: string) " + chain +
           "  end constructor; "
           "end K; "
           "class C "
           "  public static method Main(): int "
           "    Console.WriteLine(new K(\"abcd\").Tag); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// Row 1 (the issue's s1): `y` names only a PARENT parameter; `this(...)`
// chains within the same type (§12.8), so nothing binds.
BEZEL_TEST(ctor_chain_applicability_4854, rejects_s1_parent_param_name_spec_12_8) {
    auto r = runAot(childProgram(kParentY, "this(y := 42); "));
    expectRejected(r,
                   "binds no other constructor of `Child`: no other "
                   "constructor accepts these arguments by parameter name "
                   "and type");
}

// Row 2 (the issue's s5): a positional argument after a named one, and no
// Child constructor has `x` (§22.5). Pre-#4824 this SIGSEGV'd.
BEZEL_TEST(ctor_chain_applicability_4854,
           rejects_s5_positional_after_named_spec_22_5) {
    auto r = runAot(childProgram(kParentY, "this(x := 99, s); "));
    expectRejected(r,
                   "`this(...)` constructor chain call in `Child`: no other "
                   "constructor of `Child` can bind these arguments");
}

// Row 3 (the issue's baseline): `this(1, 2)` with no arity-2 constructor.
BEZEL_TEST(ctor_chain_applicability_4854, rejects_this_no_arity_match_spec_22_5) {
    auto r = runAot(childProgram(kParentY, "this(1, 2); "));
    expectRejected(r,
                   "`this(...)` constructor chain call in `Child`: no other "
                   "constructor of `Child` can bind these arguments");
}

// Row 4 (the issue's base twin): `base(z := 1)`, no base parameter `z`.
BEZEL_TEST(ctor_chain_applicability_4854, rejects_base_unknown_name_spec_22_5) {
    auto r = runAot(childProgram(kParentY, "base(z := 1); "));
    expectRejected(r,
                   "`base(...)` constructor chain call in `Child` has no "
                   "parameter named `z` (§22.5)");
}

// Row 5: `base("x")` binds `Parent(y: int)` by arity, but a `string` is not
// §5.1-convertible to `int`. Was: accepted, printed `hello/0`.
BEZEL_TEST(ctor_chain_applicability_4854,
           rejects_base_positional_wrong_type_spec_5_1) {
    auto r = runAot(childProgram(kParentY, "base(\"x\"); "));
    expectRejected(r,
                   "the argument `y` to `base(...)` constructor chain call in "
                   "`Child` has a type that is not implicitly convertible to "
                   "the target type (§5.1)");
}

// Row 6: the named spelling of row 5.
BEZEL_TEST(ctor_chain_applicability_4854, rejects_base_named_wrong_type_spec_5_1) {
    auto r = runAot(childProgram(kParentY, "base(y := \"x\"); "));
    expectRejected(r,
                   "the argument `y` to `base(...)` constructor chain call in "
                   "`Child` has a type that is not implicitly convertible to "
                   "the target type (§5.1)");
}

// Row 7: `this(s, 2)` binds the sibling `(x: int := 1, y: int := 0)` by
// arity, but `s: string` is not §5.1-convertible to `x: int`.
BEZEL_TEST(ctor_chain_applicability_4854, rejects_this_wrong_type_spec_5_1) {
    auto r = runAot(kProgram("this(s, 2); "));
    expectRejected(r,
                   "the argument `x` to `this(...)` constructor chain call in "
                   "`K` has a type that is not implicitly convertible to the "
                   "target type (§5.1)");
}

// Row 8: two base candidates bind `base(true)`; neither is applicable
// (§22.6 stage 8, the no-candidate error).
BEZEL_TEST(ctor_chain_applicability_4854,
           rejects_base_no_applicable_overload_spec_22_6) {
    auto r = runAot(childProgram(kParentIntOrString, "base(true); "));
    expectRejected(r,
                   "no overload of `base(...)` constructor chain call in "
                   "`Child` is applicable to these arguments");
}

// Row 9: the `this(...)` twin of row 8.
BEZEL_TEST(ctor_chain_applicability_4854,
           rejects_this_no_applicable_overload_spec_22_6) {
    auto r = runAot(overloadProgram(
        "  public constructor(x: int) Tag := \"int\"; end constructor; "
        "  public constructor(x: long) Tag := \"long\"; end constructor; ",
        "this(true); "));
    expectRejected(r,
                   "no overload of `this(...)` constructor chain call in `K` "
                   "is applicable to these arguments");
}

// Row 10: `this(3)` against `(x: long)` / `(x: double)` — both need a §5.1
// widening, neither is better (§22.6 stage 8, the ambiguity error), exactly
// as `new K(3)` is.
BEZEL_TEST(ctor_chain_applicability_4854, rejects_this_ambiguous_spec_22_6) {
    auto r = runAot(overloadProgram(
        "  public constructor(x: long) Tag := \"long\"; end constructor; "
        "  public constructor(x: double) Tag := \"double\"; end constructor; ",
        "this(3); "));
    expectRejected(r,
                   "ambiguous call to `this(...)` constructor chain call in "
                   "`K`: no overload is unambiguously better (§22.6)");
}

// Row 11: the issue's valid shape — `this(y := s.Length)` binds the
// all-defaulted sibling by name; `x` takes its default.
BEZEL_TEST(ctor_chain_applicability_4854, accepts_issue_valid_shape_spec_22_5) {
    expectRan(runAot(kProgram("this(y := s.Length); ")), "XY1,4\n");
}

// Row 12: `base("a")` against `(y: int)` / `(y: string)` — the string
// overload is the unique applicable candidate.
BEZEL_TEST(ctor_chain_applicability_4854,
           accepts_base_overload_by_type_spec_22_6) {
    expectRan(runAot(childProgram(kParentIntOrString, "base(\"a\"); ")),
              "hello/1\n");
}

// Row 13: `this(3)` against `(x: int)` / `(x: long)` — the exact match wins.
BEZEL_TEST(ctor_chain_applicability_4854, accepts_this_exact_match_wins_spec_22_6) {
    expectRan(runAot(overloadProgram(
                  "  public constructor(x: int) Tag := \"int\"; end constructor; "
                  "  public constructor(x: long) Tag := \"long\"; end constructor; ",
                  "this(3); ")),
              "int\n");
}

// Row 14: a well-typed `base(s.Length)` into the lone base constructor.
BEZEL_TEST(ctor_chain_applicability_4854, accepts_base_well_typed_spec_12_8) {
    expectRan(runAot(childProgram(kParentY, "base(s.Length); ")), "hello/5\n");
}

// Row 15: a `this(...)` chain inside a generic class, passing the class's
// own `T` through — the parameter's `T` is the argument's `T`.
BEZEL_TEST(ctor_chain_applicability_4854,
           accepts_this_in_generic_class_spec_14_1) {
    expectRan(runAot("module M; "
                     "class Box<T> "
                     "  public V: T; "
                     "  public N: int; "
                     "  public constructor(v: T, n: int) V := v; N := n; "
                     "  end constructor; "
                     "  public constructor(v: T) this(v, 7); end constructor; "
                     "end Box; "
                     "class C "
                     "  public static method Main(): int "
                     "    var b := new Box<string>(\"q\"); "
                     "    Console.WriteLine($\"{b.V}{b.N}\"); "
                     "    return 0; "
                     "  end Main; "
                     "end C;"),
              "q7\n");
}
