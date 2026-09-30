// Regression tests for #4975 (§22.4 "Inline `out` declarations" / §28.3
// "Unwind disposal of owned parameters" / §28.9 "Runtime-managed types" /
// §28.11 "Out-owned recipients" / §12.8 "Constructor chaining", v0.307):
// a leading `this(...)` / `base(...)` constructor chain call whose recipient
// is an inline `out owned var x` / `out using var x` / `out owned _`
// declaration hit the generic "internal: lowerExpr returned null for
// base-constructor argument" #1386 fallback in `Emitter::emitChainCall`
// (LlvmBackend_Setup.cpp) — a follow-up to #1386, whose fix covered the
// ordinary `out var` and pre-declared `out owned` spellings but never the
// two `out owned`-family inline forms at a chain-call site.
//
// `out owned _` (the discard spelling) is confirmed UNREACHABLE at codegen:
// the parser (`src/parser/Parser_Decls.cpp`, ~line 2916-3009) requires
// `out owned` to be followed by `var` and then rejects `out owned var _` at
// PARSE time ("it would declare nothing"), so no codegen test exists for
// that shape — narrowing this fix (and this file) to the two reachable
// forms, `out owned var` and `out using var`.
//
// MEASURED ROOT CAUSE (pre-fix, at this file's base commit). `isByRefMod`
// already lists "out owned var" / "out using var" among the by-ref-alias
// modifiers, so `emitChainCall`'s per-argument loop tried the `lvalueAddr`
// alias path for the fresh recipient identifier — which has no existing
// slot to alias — leaving `v` null and falling into the #1386 ICE guard.
// Two DEEPER gaps sat behind that first one, found while proving the fix
// end-to-end rather than merely silencing the ICE:
//
//   (1) `emitChainCall` runs BEFORE the constructor body's root
//       `disposeStack_` scope exists (`emitConstructorBody` clears
//       `disposeStack_` and then runs the chain call, with the first
//       `lowerScopedBlock` — which pushes the root scope — coming many
//       statements later), so a Disposal built at chain-call time has
//       nowhere to register. Fixed by a new root-scope registration
//       channel, `pendingChainOutDisposals_`, mirroring the existing
//       `pendingOwnedParams_` / `pendingArgsDisposal_` pattern for the
//       identical "no scope open yet" reason, drained by the first
//       `lowerScopedBlock` call.
//   (2) SEMA never resolved a `declType` for a chain-call `out owned var` /
//       `out using var` recipient at all: `ce->callee` for a chain call is
//       the bare keyword identifier `this`/`base`, which
//       `ctx_.exprSymbol(*ce->callee)` resolves to no ordinary symbol, so
//       the generic out-var registration block's `calleeMd2`/`calleeMs2`/
//       `calleeXd2` (ExprTypecheck_Exprs.cpp) stayed null and
//       `formalOutParamType2` always answered null for a chain call. With
//       `recipTy` null, `outOwnedRecipientDisposes(nullptr)` is always
//       false, so — even with (1) fixed — NO disposal was ever registered:
//       an `out using var` chain recipient's unconditional §28.9 disposal
//       obligation was silently never discharged (measured: 16 B leaked
//       per construction under ASan/LSan, and the destructor never ran).
//       Fixed by resolving the chain's bound target constructor the same
//       way `checkChainCallBinding` (ExprTypecheck_Calls.cpp) does — `this`
//       to the enclosing type, `base` to its first class base, same-arity
//       candidate — and reading its formal type at the recipient's
//       argument position directly.
//
// Both gaps are exactly what the issue's own text warns a shallow fix would
// miss: "A fix that only allocates a scratch slot ... without this
// bookkeeping risks a double-free or a leaked owned value."

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::JitResult;

namespace {

// Disposal-counted class: `_n` is the live-instance count, read back through
// `AtomicInteger<int>` (a class type — v0.221 §22.12 rule 2 form (iii)
// rejects a mutable static primitive).
const char* kCounted =
    "class Counted4975 "
    "  private static readonly _n: AtomicInteger<int> := new AtomicInteger<int>(0); "
    "  public static method Live(): int => _n.Value; "
    "  public constructor() _n.Value := _n.Value + 1; end constructor; "
    "  destructor _n.Value := _n.Value - 1; end destructor; "
    "end Counted4975; ";

// ---------------------------------------------------------------------------
// (a) `base(out owned var recipient)` — the issue's own repro shape, plus an
// explicit `recipient.Dispose()` (the §28.3 non-auto-drop discharge form).
// Pre-fix: "internal: lowerExpr returned null for base-constructor argument"
// — the compile itself is rejected (r.ran == false). Post-fix: compiles,
// runs, and each construction is exactly balanced (Live() == 0 after).
// ---------------------------------------------------------------------------

std::string baseOutOwnedVarProgram() {
    return std::string("module M; ") + kCounted +
        "class Base4975 "
        "  public constructor(out owned r: Counted4975) r := new Counted4975(); "
        "  end constructor; "
        "end Base4975; "
        "class Derived4975oo : Base4975 "
        "  public constructor() "
        "    base(out owned var recipient); "
        "    recipient.Dispose(); "
        "  end constructor; "
        "end Derived4975oo; "
        "class Program4975oo "
        "  public static method Main(): int "
        "    var d := new Derived4975oo(); "
        "    d.Dispose(); "
        "    Console.WriteLine(Counted4975.Live()); "
        "    return 0; "
        "  end Main; "
        "end Program4975oo;";
}

BEZEL_TEST(chain_call_out_owned_var_recipient_4975,
           base_chain_out_owned_var_explicit_dispose) {
    auto r = runAot(baseOutOwnedVarProgram());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// Same shape via `this(...)`: a convenience constructor of the SAME class
// chains to a sibling declaring the `out owned` parameter. `emitChainCall`'s
// `this`/`base` distinction (isThis) is exercised identically by both new
// arms this fix added, so both chain spellings get their own row.
std::string thisOutOwnedVarProgram() {
    return std::string("module M; ") + kCounted +
        "class WithSibling4975 "
        "  public constructor(out owned r: Counted4975) r := new Counted4975(); "
        "  end constructor; "
        "  public constructor(unused: int) "
        "    this(out owned var recipient); "
        "    recipient.Dispose(); "
        "  end constructor; "
        "end WithSibling4975; "
        "class Program4975this "
        "  public static method Main(): int "
        "    var d := new WithSibling4975(1); "
        "    d.Dispose(); "
        "    Console.WriteLine(Counted4975.Live()); "
        "    return 0; "
        "  end Main; "
        "end Program4975this;";
}

BEZEL_TEST(chain_call_out_owned_var_recipient_4975,
           this_chain_out_owned_var_explicit_dispose) {
    auto r = runAot(thisOutOwnedVarProgram());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// ---------------------------------------------------------------------------
// (b) `base(out using var recipient)` — the §28.9 SCOPE-BOUND form: no
// explicit `.Dispose()` call anywhere; the recipient must be disposed
// automatically at the end of the chaining constructor's body. This is the
// row that isolates gap (2) above — pre-(2)-fix (but with the ICE already
// silenced) the program compiled and ran clean, but LEAKED the recipient
// (Live() stayed 1, and under ASan/LSan a 16-byte direct leak was reported
// from `Base4975$ctor`, confirmed via a scratch probe copy of this repro).
// ---------------------------------------------------------------------------

std::string baseOutUsingVarProgram() {
    return std::string("module M; ") + kCounted +
        "class Base4975u "
        "  public constructor(out owned r: Counted4975) r := new Counted4975(); "
        "  end constructor; "
        "end Base4975u; "
        "class Derived4975uv : Base4975u "
        "  public constructor() "
        "    base(out using var recipient); "
        "  end constructor; "
        "end Derived4975uv; "
        "class Program4975uv "
        "  public static method Main(): int "
        "    var d := new Derived4975uv(); "
        "    d.Dispose(); "
        "    Console.WriteLine(Counted4975.Live()); "
        "    return 0; "
        "  end Main; "
        "end Program4975uv;";
}

BEZEL_TEST(chain_call_out_owned_var_recipient_4975,
           base_chain_out_using_var_auto_dispose_single_shot) {
    JitResult r = runAotChecked(baseOutUsingVarProgram());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

// 500x amplification under ASan/LSan — the FAIL-WITHOUT-strength oracle for
// gap (2): a one-shot leak of 16 B can be lost in slack; 500 * 16 B = 8000 B
// is not. Also functionally asserts Live() == 0 after all 500 constructions.
BEZEL_TEST(chain_call_out_owned_var_recipient_4975,
           base_chain_out_using_var_auto_dispose_500x_leak_free) {
    std::string prog = std::string("module M; ") + kCounted +
        "class Base4975u2 "
        "  public constructor(out owned r: Counted4975) r := new Counted4975(); "
        "  end constructor; "
        "end Base4975u2; "
        "class Derived4975uv2 : Base4975u2 "
        "  public constructor() "
        "    base(out using var recipient); "
        "  end constructor; "
        "end Derived4975uv2; "
        "class Program4975uv2 "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var d := new Derived4975uv2(); "
        "      d.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(Counted4975.Live()); "
        "    return 0; "
        "  end Main; "
        "end Program4975uv2;";
    JitResult r = runAotChecked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}

}  // namespace
