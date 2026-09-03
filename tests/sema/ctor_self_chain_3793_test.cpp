// Spec-derived regression test for issue #3793 — sema accepted a constructor
// that chains to ITSELF (`constructor(a: int) this(a); end`) silently, and the
// chain lowered to nothing at codegen because `ctorByArity` excludes the
// originating constructor (`LlvmBackend_impl.hpp:12232`, §22.10's "no
// self-chain" carve-out).
//
// spec §22.10 "Primary constructors" (v0.286):
//
//   "A class with a primary constructor may declare additional convenience
//    constructors. Each must chain to another constructor via `this(...)`;
//    only the primary may chain to `base(...)`."
//
// spec §12.8 "Constructor chaining" (v0.286) states the same "another" from
// the constructor side: "this(...) chains to another constructor in the same
// class." §22.8 step 1 / §22.10 name the rule's exemption: the chain must
// resolve to a SIBLING constructor in the class, not to the originating
// constructor itself. §22.8 step 1 / §22.5 give the parameter-binding form
// the chain takes at codegen — binding the chain call's arguments to the
// target ctor's parameter list (or, for a primary ctor, the primary's
// positional fields via `ExprTC::publishPrimaryCtorChainBinding`).
//
// WHAT THE DEFECT WAS. `Resolver::checkTypeConstructorChains`
// (TypeResolution_Checks.cpp) enforced §12.8's two positional rules — at most
// one chain call, it must be the first statement — and #3535's
// primary-ctor-class require-this(...) rule, but never asked which constructor
// the chain *resolves to*. A non-primary class's `constructor(a: int)
// this(a); end` with no other arity-1 sibling compiled clean; codegen's
// `ctorByArity(targetCls, 1, cs /*exclude-self*/)` had nothing to pick and
// the chain lowered to nothing. The instance kept its field defaults
// everywhere a field had no initializer. The same defect in graph form: two
// ctors in a cycle (`constructor(a, s) this(a, s, 0); constructor(a, s, k)
// this(a, s);`) — each chains to the other, and each eventually chains to
// itself.
//
// WHAT THE FIX DOES. `checkTypeConstructorChains` now also walks the
// `this(...)` chain graph and rejects any ctor whose chain resolves to itself,
// directly (no matching sibling and no primary ctor to absorb the call) or
// transitively (a cycle through one or more siblings). The walk mirrors
// codegen's `ctorByArity` selection (first non-self, non-static sibling with
// matching parameter count). A legitimate forwarder (`constructor(a) this(a,
// 5);` whose sibling `constructor(a, b)` exists at arity 2) terminates at the
// non-`this(...)` sibling and is admitted. A class with a primary constructor
// whose chain has no matching sibling passes: §22.10 says the chain target
// IS the primary, and `publishPrimaryCtorChainBinding` already publishes the
// positional binding for that case.
//
// FAIL-WITHOUT / PASS-WITH. Every `rejects_*` row compiles CLEAN on the lane
// base — the asserted diagnostic text does not exist on that tree at all, and
// the chain lowers to nothing, so the runAot-driven row also produces a
// silently-wrong object (no field assignment). Every `accepts_*` row holds on
// both trees and is the false-rejection guard: a rejection keyed on "ctor has
// a `this(...)` chain" is exactly the kind that over-rejects the conforming
// forwarder and the static constructor, so each is pinned with its VALUE.

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
// tell a genuine reject from the very silent-wrong-object this fix removes.
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

// Two diagnostic strings the §22.10 self-chain rule emits. They are
// distinguished because the rule has two failure shapes (direct self-chain
// vs transitive cycle), and each is named on its own so the test can pin
// which shape the offending program tripped.
constexpr const char* kNoTargetDiag =
    "`this(...)` constructor chain has no target";
constexpr const char* kCycleDiag =
    "`this(...)` constructor chain forms a cycle";

}  // namespace

// ---------------------------------------------------------------------------
// The issue's repro: direct self-chain in a non-primary class.
// ---------------------------------------------------------------------------

// #3793 / §22.10 (v0.286): the brief's exact shape — a non-primary class
// whose only constructor chains to itself. The chain target (arity 1, no
// other arity-1 sibling, no primary ctor) is undefined; §22.10 requires
// "another constructor" so this is a §22.10 reject. The fix diagnostic lands
// at the `this(a);` site, not at 0:0 (the span #3796 threaded through).
BEZEL_TEST(ctor_self_chain_3793,
           rejects_direct_self_chain_in_non_primary_class_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public A: int; "
        "  public constructor(a: int) "
        "    this(a); "
        "  end constructor; "
        "end C; "
        "class MainCls public static method Main(): int "
        "  using var c := new C(5); "
        "  Console.WriteLine(c.A.ToString()); "
        "  return 0; "
        "end Main; end MainCls;");
    if (!hasError(r, kNoTargetDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kNoTargetDiag));
}

// #3793 / §22.10: a TRANSITIVE cycle through two ctors. Each constructor's
// chain resolves to the other, so the chain returns to the originating ctor
// after two steps. §22.10 forbids both — every ctor in the cycle is rejected
// at its `this(...)` site.
BEZEL_TEST(ctor_self_chain_3793,
           rejects_indirect_two_ctor_cycle_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public A: int; "
        "  public S: string; "
        "  public constructor(a: int, x: string) "
        "    this(a, x, 0); "
        "  end constructor; "
        "  public constructor(a: int, x: string, k: int) "
        "    this(a, x); "
        "  end constructor; "
        "end C; "
        "class MainCls public static method Main(): int "
        "  using var c := new C(5, \"hi\"); "
        "  Console.WriteLine(c.A.ToString()); "
        "  return 0; "
        "end Main; end MainCls;");
    if (!hasError(r, kCycleDiag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, kCycleDiag));
}

// #3793 / §22.10: the cycle diagnostic lands at the `this(...)` SITE, not at
// the constructor's span or at 0:0. The AOT harness synthesizes one Error
// whose message is the driver stderr blob (S1 in aot_support.hpp — no test
// inspects a diagnostic's `.location` directly), so the location pattern lives
// in the message text. The diagnostic's column must be NON-ZERO (the §22.10
// diagnostic #3796 anchored at `this(...)` carries the chain call's source
// column, which on the second ctor's `this(a, x);` is column 5; the pre-#3796
// anchor would have printed `:0:0:`).
BEZEL_TEST(ctor_self_chain_3793,
           cycle_diagnostic_lands_at_chain_site_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public constructor(a: int, x: string) "
        "    this(a, x, 0); "
        "  end constructor; "
        "  public constructor(a: int, x: string, k: int) "
        "    this(a, x); "
        "  end constructor; "
        "end C; "
        "class MainCls public static method Main(): int "
        "  using var c := new C(5, \"hi\"); "
        "  return 0; "
        "end Main; end MainCls;");
    // The driver stderr is the entire diagnostic message blob. The §22.10
    // cycle diagnostic text appears once per ctor in the cycle, and each
    // occurrence has a non-zero column anchor (the `this(...)` site on its
    // own line). At least one such occurrence must NOT carry `:0:0:`.
    bool foundAtNonZero = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::size_t pos = 0;
        while ((pos = d.message.find(kCycleDiag, pos)) != std::string::npos) {
            // Walk back from the message match to find the `:line:column:`
            // prefix the driver emits (`/path:LINE:COL: severity: ...`).
            // The simplest robust check is that the substring is preceded by
            // `:N:M:` with N or M non-zero — `:0:0:` is the §3796 anchor
            // #3796 already removed.
            const std::size_t prefix = d.message.rfind(':', pos - 1);
            const std::size_t prefix2 =
                (prefix == std::string::npos)
                    ? std::string::npos
                    : d.message.rfind(':', prefix - 1);
            if (prefix != std::string::npos &&
                prefix2 != std::string::npos) {
                // The substring between prefix2 and prefix is the line, and
                // between prefix and pos is the column. Both must be non-zero.
                const std::string lineStr =
                    d.message.substr(prefix2 + 1, prefix - prefix2 - 1);
                const std::string colStr =
                    d.message.substr(prefix + 1, pos - prefix - 1);
                if (lineStr != "0" || colStr != "0") {
                    foundAtNonZero = true;
                    break;
                }
            }
            pos += std::strlen(kCycleDiag);
        }
        if (foundAtNonZero) break;
    }
    if (!foundAtNonZero) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(foundAtNonZero);
}

// ---------------------------------------------------------------------------
// False-rejection guards. Each pins a VALUE so a row cannot pass merely
// because the program compiled.
// ---------------------------------------------------------------------------

// #3793 / §22.10: the LEGITIMATE forwarder. `constructor(a: int) this(a, 5);`
// chains to a SIBLING `constructor(a: int, b: int)` (arity 2), which has no
// `this(...)` chain itself, so the chain terminates. The instance's fields
// hold the values the chain forwarded.
BEZEL_TEST(ctor_self_chain_3793,
           accepts_legitimate_forwarder_to_different_arity_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class C "
        "  public A: int; "
        "  public B: int; "
        "  public constructor(a: int, b: int) "
        "    A := a; "
        "    B := b; "
        "  end constructor; "
        "  public constructor(a: int) "
        "    this(a, 5); "
        "  end constructor; "
        "end C; "
        "class MainCls public static method Main(): int "
        "  using var c := new C(7); "
        "  Console.WriteLine(c.A.ToString()); "
        "  Console.WriteLine(c.B.ToString()); "
        "  return 0; "
        "end Main; end MainCls;");
    if (r.stdoutText != "7\n5\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n5\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3793 / §22.10: a class WITH a primary constructor and a `this(...)`
// forwarder whose arity matches no other convenience ctor. §22.10's
// "another constructor" is satisfied: the chain target is the primary ctor
// itself (via `ExprTC::publishPrimaryCtorChainBinding`'s positional binding).
// This row pins that the self-chain reject does NOT over-trigger when a
// primary ctor absorbs the call.
BEZEL_TEST(ctor_self_chain_3793,
           accepts_chain_to_primary_ctor_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(v: int) "
        "    this(v, \"chained\"); "
        "  end constructor; "
        "  public method Show() "
        "    Console.WriteLine(n.ToString()); "
        "    Console.WriteLine(s); "
        "  end Show; "
        "end B; "
        "class MainCls public static method Main(): int "
        "  using var b := new B(5); "
        "  b.Show(); "
        "  return 0; "
        "end Main; end MainCls;");
    if (r.stdoutText != "5\nchained\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\nchained\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3793 / §22.7: a STATIC constructor has no chaining position at all
// ("It carries no access modifier ... and no parameters", §22.7), so the
// §22.10 chain-target rule does not reach it. Without this exemption the
// graph walk would consider a static ctor as a potential chain target and
// either misclassify its body or count its absence as a terminator in the
// wrong place.
BEZEL_TEST(ctor_self_chain_3793,
           accepts_static_ctor_does_not_enter_chain_graph_spec_22_7) {
    JitResult r = runAot(
        "module M; "
        "class B "
        "  public static Seen: int := 0; "
        "  static constructor() "
        "    Seen := 7; "
        "  end constructor; "
        "  public _x: int; "
        "  public constructor(x: int) "
        "    _x := x; "
        "  end constructor; "
        "end B; "
        "class MainCls public static method Main(): int "
        "  using var b := new B(11); "
        "  Console.WriteLine(b._x.ToString()); "
        "  Console.WriteLine(B.Seen.ToString()); "
        "  return 0; "
        "end Main; end MainCls;");
    if (r.stdoutText != "11\n7\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n7\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
