// Spec-derived regression test for issue #2833.
//
// §23.4 (Indexers) / §27.3 (Lambda expressions) / §12.1 (Members are
// non-virtual by default), v0.240.
//
// §23.4 states, verbatim:
//
//     "Inside a member of a class with its own indexer, `this[i]` is required
//      (the brackets need a left-hand side)."
//
// so `this[i]` is the spelling the spec MANDATES. Inside a LAMBDA body it
// lowered to 0 (a read) and the write was silently DISCARDED, with no
// diagnostic once #2795 gave the expression a type. Same in an `async` body.
//
// ROOT CAUSE. `Emitter::indexReceiverDecl` (src/codegen/LlvmBackend_Assign.cpp)
// recovered the receiver TypeSymbol for a `this`/`base`-rooted index by a
// REVERSE LOOKUP of `currentFn_` in `methodFns_` (the #747 R42 branch). Inside
// a lambda body `currentFn_` is the CLOSURE's llvm::Function, which is not a
// value in that map (and `currentSpecMethodSymbol_` is unset); inside an
// `async` body it is the state-machine frame's, likewise absent. The lookup
// failed, `indexReceiverDecl` returned null, and all THREE callers bail on
// null silently — `lowerIndexerGet` returns null (the read degrades to a
// zero/null value through the #2434 / #2491 fallbacks), `lowerIndexerSet`
// returns false (the write is discarded), `lowerIndexerCompound` returns false
// ("silently a no-op (returns old value, no store)", its own header).
//
// THE FIX (`Emitter::indexAccessReceiverDecl`, same file) reads the indexer
// PropertySymbol that sema attaches to the IndexExpr (#2530) when the
// llvm::Function-based recovery fails. That is CATEGORY-BLIND by construction —
// it asks the type-checker what this very access resolved to rather than
// enumerating codegen body kinds — which is why one line covers the
// expression-bodied, `do`-bodied, directly-passed and nested lambda, the
// `async` body, and the generic/interface/inherited receivers alike.
//
// WHAT THIS FILE REPLACES. `tests/lit/this_indexer_dropped_in_lambda_warns_
// 2833.bzl` pinned the WARNING that announced the drop. Its own maintenance
// note said: "When #2833 is fixed … the CHECK stops matching. That is a CORRECT
// signal, and the right response is to delete this file and replace it with a
// VALUE test in the #2833 fix (the lambda below must then return 91), NOT to
// weaken the assertion." Verified mechanically: that file's `this[i]`-in-a-
// lambda no longer draws the `#2833` line. It is deleted here and every one of
// its accept controls is carried over below as a VALUE case, plus T13, which
// asserts the announcement is silent — so the guard's intent (no shape may be
// both broken and silent) is still enforced, now from the value side.
//
// Every case asserts a CORRECT VALUE, not a clean compile: the defect compiled
// cleanly and produced 0.
//
// Related: #2795 (typed `this[i]`; unmasked the silence here), #2832 (the
// `base[i]` mis-dispatch, fixed in the companion commit), #2530 (the
// PropertySymbol attachment this fix reads), #2836 (compound assignment
// through a VIRTUAL indexer is statically bound — pre-existing, filed, and
// why T6's expectation is the statically-bound value).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

using bezel::test::runAot;
using bezel::test::runAotOut;

int run(const bezel::test::JitResult& r) { return r.ran ? r.exitCode : -999; }

// True iff some reported diagnostic contains `sub`.
bool hasDiag(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The shared declaring class: a read-write `[i: int]: int` indexer over a
// mutable field, so a read, a `:=` write and a `+=` compound are all
// observable through the SAME accessor pair.
constexpr const char* kBox =
    "class Box "
    "  private _v: int := 0; "
    "  public property [i: int]: int "
    "    get => i + 90 + _v; "
    "    set => do _v := value; end; "
    "  end; ";

}  // namespace

// ---------------------------------------------------------------------------
// THE ISSUE'S OWN REPRO — read through `this[i]` in an expression-bodied
// lambda. Pre-fix: 0 (measured on 6eaaecf, and identically before #2795).
// Post-fix: 91.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           read_through_expression_bodied_lambda_23_4) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public method ViaIdx(): int "
        "    var f := lambda(n: int) => this[n]; "
        "    return f(1); "
        "  end ViaIdx; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.ViaIdx(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 91);
}

// #2833 / §27.3 — a `do`-bodied lambda. Listed on the issue as a separately
// measured dropped shape (0 pre-fix).
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           read_through_do_bodied_lambda_27_3) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public method ViaIdx(): int "
        "    var f := lambda(n: int) => do return this[n]; end; "
        "    return f(1); "
        "  end ViaIdx; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.ViaIdx(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 91);
}

// #2833 / §27.3 — a NESTED lambda. The inner closure's llvm::Function is not
// in `methodFns_` either, and neither is its enclosing one, so a fix that only
// remembered "the method one level out" would have missed this.
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           read_through_nested_lambda_27_3) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public method ViaIdx(): int "
        "    var f := lambda(n: int) => do "
        "      var g := lambda(m: int) => this[m]; "
        "      return g(n); "
        "    end; "
        "    return f(1); "
        "  end ViaIdx; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.ViaIdx(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 91);
}

// #2833 / §27.3 — a lambda passed DIRECTLY as a call argument, never bound to
// a local. Also listed on the issue as separately measured.
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           read_through_directly_passed_lambda_27_3) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public method ViaIdx(): int "
        "    return C.Apply(lambda(n: int) => this[n], 1); "
        "  end ViaIdx; "
        "end Box; "
        "class C "
        "  public static method Apply(f: (int) -> int, n: int): int => f(n); "
        "  public static method Main(): int "
        "    using var b := new Box(); return b.ViaIdx(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(run(r), 91);
}

// ---------------------------------------------------------------------------
// THE WRITE. `this[i] := v` inside a lambda was SILENTLY DISCARDED — the slot
// kept its old value. This was NOT in the original report; the #2795 follow-up
// measured it through the guard and added it to the issue.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           set_through_lambda_is_not_discarded_23_4) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public method W(): int "
        "    var f := lambda(n: int) => do this[0] := n; end; "
        "    f(5); "
        "    return this[0]; "        // 0 + 90 + 5
        "  end W; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.W(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 95);
}

// #2833 / §23.4 — the COMPOUND path (`lowerIndexerCompound`, the third caller
// that bails on a null receiver). Pre-fix: the read AND the store were both
// dropped, so `_v` stayed 0 and the method returned 90.
// The expectation is the STATICALLY-bound value (get 90, +3 = 93, set _v:=93,
// then read 0+90+93): `Box`'s indexer is non-virtual, so static binding IS the
// correct lowering here. #2836 tracks the separate, pre-existing defect that
// the compound path stays statically bound even for a VIRTUAL indexer.
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           compound_through_lambda_is_not_a_no_op_23_4) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public method W(): int "
        "    var f := lambda(n: int) => do this[0] += n; end; "
        "    f(3); "
        "    return this[0]; "
        "  end W; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.W(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 183);
}

// ---------------------------------------------------------------------------
// THE ASYNC BODY — a state-machine frame, not a closure. This is the shape an
// `inLambda_`-style SOURCE-CATEGORY guard would have missed outright, which is
// why both the announcement and this fix are gated on the recovery itself.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           read_inside_async_body_30_1) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public async method A(): Task<int> "
        "    return this[1]; "
        "  end A; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.A().Result; "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 91);
}

// #2833 / §23.4 — a STRING-typed indexer result read in a lambda. The issue
// reports the string variant returning the EMPTY string; this pins the
// reference-typed result (ownership/refcount, not just an integer).
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           string_result_through_lambda_23_4) {
    auto r = runAotOut(
        "module M; "
        "class Names "
        "  public property [k: string]: string => k + \"!\"; "
        "  public method ViaIdx(): string "
        "    var f := lambda(k: string) => this[k]; "
        "    return f(\"ab\"); "
        "  end ViaIdx; "
        "end Names; "
        "class C public static method Main(): int "
        "  using var n := new Names(); Console.WriteLine(n.ViaIdx()); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "ab!");
}

// ---------------------------------------------------------------------------
// VIRTUAL DISPATCH MUST SURVIVE. The fix answers with the indexer's DECLARING
// type, which for an INHERITED indexer is a BASE of the runtime receiver — so
// the load-bearing question is whether the access still dispatches to the
// most-derived override. `Mid` declares no indexer; the lambda lives in `Mid`;
// the runtime object is a `Leaf` that overrides. §12.1: a `virtual` member
// dispatches on the runtime type.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           inherited_indexer_in_lambda_still_dispatches_virtually_12_1) {
    // ⚠ runAot's exitCode is WEXITSTATUS-masked to 8 bits, so the two-value
    // observable goes through stdout (runAotOut), not the exit code.
    auto r = runAotOut(
        "module M; "
        "class Root "
        "  public virtual property [i: int]: int => i + 40; "
        "end Root; "
        "class Mid: Root "
        "  public method ViaIdx(i: int): int "
        "    var f := lambda(n: int) => this[n]; "
        "    return f(i); "
        "  end ViaIdx; "
        "end Mid; "
        "class Leaf: Mid "
        "  public override property [i: int]: int => i + 50; "
        "end Leaf; "
        "class C public static method Main(): int "
        "  using var lf := new Leaf(); using var md := new Mid(); "
        "  Console.WriteLine(lf.ViaIdx(1) * 100 + md.ViaIdx(1)); "  // 51*100+41
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r == "5141");
}

// #2833 / §14.2 + §23.4 — a GENERIC declaring class. The receiver recovery must
// resolve to the template TypeSymbol so the monomorphized body indexes its own
// instantiation.
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           generic_declaring_class_in_lambda_14_2) {
    auto r = runAot(
        "module M; "
        "class G<T> "
        "  private _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  public property [i: int]: T => _v; "
        "  public method ViaIdx(): T "
        "    var f := lambda(n: int) => this[n]; "
        "    return f(0); "
        "  end ViaIdx; "
        "end G; "
        "class C public static method Main(): int "
        "  using var g := new G<int>(42); return g.ViaIdx(); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 42);
}

// #2833 / §11.2 + §23.4 — the class's indexer also SATISFIES an interface
// declaration. The declaring type the fix answers with must still be the CLASS
// (whose own member the resolver finds first), not the bodiless interface
// indexer — an interface receiver would take the itable path.
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           interface_declared_indexer_in_lambda_11_2) {
    auto r = runAot(
        "module M; "
        "interface IIdx readonly property [i: int]: int; end IIdx; "
        "class Impl: IIdx "
        "  public property [i: int]: int => i + 70; "
        "  public method ViaIdx(i: int): int "
        "    var f := lambda(n: int) => this[n]; "
        "    return f(i); "
        "  end ViaIdx; "
        "end Impl; "
        "class C public static method Main(): int "
        "  using var im := new Impl(); "
        "  var iv: IIdx := im; "
        "  return im.ViaIdx(1) + iv[1]; "     // 71 + 71
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 142);
}

// ---------------------------------------------------------------------------
// THE SILENCE GATE (carried over from the deleted lit test's CHECK-NOT). No
// `#2833` announcement may survive for a shape the fix now recovers — that is
// what "stops firing by construction" means, and it is the assertion that
// prevents a future change from leaving a shape both broken and silent.
// Every accept control the lit file carried is exercised here in ONE program:
// a plain method body, a generic-specialization body, an extension body, and
// the lambda body itself.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           no_dropped_receiver_announcement_remains_23_4) {
    auto r = runAot(
        "module M; "
        "class Acc "
        "  public property [i: int]: int => i + 90; "
        "  public method Plain(i: int): int => this[i]; "
        "  public method Spec<T>(i: int, t: T): int => this[i]; "
        "  public method ViaLambda(i: int): int "
        "    var f := lambda(n: int) => this[n]; "
        "    return f(i); "
        "  end ViaLambda; "
        "end Acc; "
        "class Ext public property [i: int]: int => i + 40; end Ext; "
        "extension Ext public method Get(i: int): int => this[i]; end; "
        "class C public static method Main(): int "
        "  using var a := new Acc(); using var e := new Ext(); "
        "  return a.Plain(1) + a.Spec<int>(1, 0) + a.ViaLambda(1) "
        "         + e.Get(1) - 232; "    // 91 + 91 + 91 + 41 - 232 = 82
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 82);
    BEZEL_EXPECT_FALSE(hasDiag(r, "#2833"));
    BEZEL_EXPECT_FALSE(hasDiag(r, "was DROPPED"));
}

// #2833 accept control / §23.4 — a PLAIN method body was never broken and must
// stay exactly as it was (the `methodFns_` reverse lookup still answers first;
// the new fallback must not displace it).
BEZEL_TEST(codegen_this_indexer_in_lambda_2833,
           plain_method_body_control_unchanged_23_4) {
    auto r = runAot(
        std::string("module M; ") + kBox +
        "  public method Plain(i: int): int => this[i]; "
        "end Box; "
        "class C public static method Main(): int "
        "  using var b := new Box(); return b.Plain(1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(run(r), 91);
}
