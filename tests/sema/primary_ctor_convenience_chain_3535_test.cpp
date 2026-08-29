// Spec-derived regression test for issue #3535 — §22.10's convenience-
// constructor chaining rule was not enforced anywhere in sema, and the
// omission is memory-unsafe rather than merely permissive.
//
// spec §22.10 "Primary constructors" (v0.283):
//
//   "A class with a primary constructor may declare additional convenience
//    constructors. Each must chain to another constructor via `this(...)`;
//    only the primary may chain to `base(...)`."
//
// spec §22.7 "Constructors" (v0.283) states the same restriction from the
// constructor side: "When a class has a primary constructor (§22.10), an
// additional restriction applies: only the primary may chain to `base(...)`;
// convenience constructors must chain via `this(...)`."
//
// WHAT THE DEFECT WAS. `Resolver::checkTypeConstructorChains` enforced §12.8's
// two positional rules (at most one chain call; it must be the first
// statement) and nothing else, so a convenience constructor of a
// primary-constructor class that chained to NOTHING was accepted. Codegen's
// `new` site then stored the call's arguments into the primary-constructor
// field slots BY POSITION (`Emitter::storePrimaryCtorPositionalArgs`,
// LlvmBackend_Exprs2.cpp — argument i pairs with positional field i), and with
// no chain nothing ever re-stored those slots with the values the object
// should hold, so the blind pairing survived to the first field read.
//
// MEASURED ON `e57a1eb` (build/bezelc, real AOT compile + native run), with
// `class B(n: int, s: string)` and a NON-chaining `constructor(v: int,
// w: int)`:
//
//   new B(5, 9)  ->  compiles CLEAN, prints "conv-ctor", prints 5, then
//                    SIGSEGV — the integer 9 was stored into the `s: string`
//                    slot and `Show()` dereferenced it as a string.
//
// and with a non-chaining `constructor(v: int)` (arity below the primary's):
//
//   new B(5)     ->  compiles CLEAN, prints 5 then an EMPTY line — `n` took
//                    the convenience constructor's first argument by accident
//                    and `s` kept its calloc zero. A silent wrong object.
//
// WHAT THE FIX DOES. `checkTypeConstructorChains` now also rejects, for a
// class whose TypeSymbol carries `hasPositionalFields` (a §22.10 primary
// constructor — a class's primary is carried on the TypeDecl, never as a
// `ConstructorSymbol`, so EVERY declared instance constructor of such a class
// is a convenience constructor), any instance constructor whose first
// statement is not a `this(...)` chain call. The rejection is LOUD at sema,
// per the issue's direction note: the alternative — a defensive check inside
// the index-paired store — would convert a crash into a silent wrong value.
//
// FAIL-WITHOUT / PASS-WITH. Every `rejects_*` row compiles CLEAN on `e57a1eb`
// (the asserted diagnostic text does not exist on that tree at all) and two of
// them run to a SIGSEGV / wrong value there. Every `accepts_*` row holds on
// both trees and is the false-rejection guard: a rejection keyed on
// "has a primary constructor" is exactly the kind that over-rejects the
// conforming `this(...)` form, the primary-constructor-free class, and the
// static constructor, so each is pinned with its VALUE, not merely with
// "compiled".

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff SOME diagnostic contains `sub`. Rejection rows assert on SPECIFIC
// message text, never on the coarse `hasErrors()` — `runAot` synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so `hasErrors()` cannot
// tell a genuine reject from the very crash this fix removes.
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

// The §22.10 diagnostic's two halves. The second clause appears only on the
// `base(...)` arm, so asserting it separately pins that the two shapes are
// distinguished rather than collapsed into one message.
constexpr const char* kMustChainDiag =
    "must chain to another constructor via `this(...)`";
constexpr const char* kOnlyPrimaryBaseDiag =
    "only the primary constructor may chain to `base(...)`";

}  // namespace

// ---------------------------------------------------------------------------
// The issue's repro: the wild-pointer shape.
// ---------------------------------------------------------------------------

// #3535 / §22.10 (v0.283): a non-chaining convenience constructor whose
// parameter list happens to be TYPE-INCOMPATIBLE with the primary's at some
// index. On `e57a1eb` this compiled clean and SIGSEGVed dereferencing the
// integer stored into the `s: string` slot.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           rejects_non_chaining_convenience_ctor_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int, w: int) "
        "    Console.WriteLine(\"conv-ctor\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(5, 9); "
        "  b.Show(); "
        "  return 0; "
        "end Main; end C;");
    if (!hasError(r, kMustChainDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kMustChainDiag));
}

// #3535 / §22.10 (v0.283): the same rule, on the arity-below-the-primary
// shape. On `e57a1eb` this compiled clean and printed a silently wrong object
// (5 then an empty string) rather than crashing — the same defect, quiet.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           rejects_non_chaining_convenience_ctor_short_arity_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int) "
        "    Console.WriteLine(\"conv-ctor\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(5); "
        "  b.Show(); "
        "  return 0; "
        "end Main; end C;");
    if (!hasError(r, kMustChainDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kMustChainDiag));
}

// #3535 / §22.10 (v0.283): "only the primary may chain to `base(...)`". A
// convenience constructor that chains to the BASE rather than to `this(...)`
// bypasses the primary — the single-base-call rule §22.10 states in the same
// paragraph — and leaves the primary's own slots unwritten.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           rejects_base_chain_from_convenience_ctor_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class A(x: int) "
        "  public method Ax() Console.WriteLine(x.ToString()); end Ax; "
        "end A; "
        "class B(n: int): A(n) "
        "  public constructor(v: int, w: int) "
        "    base(v); "
        "    Console.WriteLine(\"conv-ctor\"); "
        "  end constructor; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(5, 9); "
        "  b.Ax(); "
        "  return 0; "
        "end Main; end C;");
    if (!hasError(r, kOnlyPrimaryBaseDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kMustChainDiag));
    BEZEL_EXPECT_TRUE(hasError(r, kOnlyPrimaryBaseDiag));
}

// #3535 / §22.10 (v0.283): an EMPTY primary parameter list still declares a
// primary constructor — `class B(): A(4)` is the form #3141 / #2455 exercise —
// so its convenience constructors owe the same `this(...)`. This row pins the
// gate on "the header carried a parameter list", not on "the list is
// non-empty"; the latter reading would leave the base-call form unguarded.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           rejects_non_chaining_ctor_with_empty_primary_param_list_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class A(x: int) "
        "  public method Ax() Console.WriteLine(x.ToString()); end Ax; "
        "end A; "
        "class B(): A(4) "
        "  public constructor(v: int) "
        "    Console.WriteLine(v.ToString()); "
        "  end constructor; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(5); "
        "  b.Ax(); "
        "  return 0; "
        "end Main; end C;");
    if (!hasError(r, kMustChainDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kMustChainDiag));
}

// ---------------------------------------------------------------------------
// False-rejection guards. Each pins a VALUE, so a row cannot pass merely
// because the program compiled.
// ---------------------------------------------------------------------------

// #3535 / §22.10 (v0.283): the CONFORMING convenience constructor. Accepted,
// and the object it builds is the one §22.8 step 0 describes — the primary's
// slots hold the `this(...)` arguments, not the `new`-site arguments the
// blind index-pairing stored first.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           accepts_this_chaining_convenience_ctor_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int, w: int) "
        "    this(v, \"chained\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(5, 9); "
        "  b.Show(); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "5\nchained\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nchained\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3535 / §22.10 (v0.283): the conforming form whose parameter ORDER differs
// from the primary's. This is the row that proves the accept is not an
// accident of the two lists lining up positionally — with the arguments
// swapped, the pre-fix blind store would have paired a string with `n: int`.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           accepts_this_chaining_convenience_ctor_reordered_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(w: string, v: int) "
        "    this(v, w); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(\"hello\", 3); "
        "  b.Show(); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "3\nhello\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\nhello\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3535 / §12.8 (v0.283): the branch-order guard. A class WITHOUT a primary
// constructor keeps §12.8's ordinary chaining surface — a `base(...)` chain
// from an explicit constructor, and an explicit constructor with no chain at
// all — untouched. Adding an arm to a classify-and-report path can re-route
// decisions that already worked, so this asserts both shapes still run.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           accepts_base_chain_in_class_without_primary_ctor_spec_12_8) {
    JitResult r = runAot(
        "module M; "
        "class A "
        "  public _x: int; "
        "  public constructor(x: int) _x := x; end constructor; "
        "end A; "
        "class B: A "
        "  public constructor(v: int) "
        "    base(v + 1); "
        "  end constructor; "
        "end B; "
        "class D "
        "  public _y: int; "
        "  public constructor() _y := 12; end constructor; "
        "end D; "
        "class C public static method Main(): int "
        "  using var b := new B(41); "
        "  Console.WriteLine(b._x.ToString()); "
        "  using var d := new D(); "
        "  Console.WriteLine(d._y.ToString()); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "42\n12\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n12\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3535 / §22.7 (v0.283): a STATIC constructor has no chaining position at
// all ("It carries no access modifier ... and no parameters"), so the §22.10
// rule does not reach it. Without this exemption the rule would reject every
// primary-constructor class that also declares a static constructor.
BEZEL_TEST(primary_ctor_convenience_chain_3535,
           accepts_static_constructor_in_primary_ctor_class_spec_22_7) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int) "
        "  public static Seen: int := 0; "
        "  static constructor() "
        "    Seen := 7; "
        "  end constructor; "
        "  public method Show() Console.WriteLine(n.ToString()); end Show; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(4); "
        "  b.Show(); "
        "  Console.WriteLine(B.Seen.ToString()); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "4\n7\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n7\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
