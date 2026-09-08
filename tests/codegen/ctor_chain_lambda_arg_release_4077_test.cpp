// Codegen/disposal — a constructor CHAIN call (`this(...)` or `base(...)`) whose
// argument is a LAMBDA LITERAL leaks the closure cell + env block + capture
// cells (issue #4077, spec §22.10 *Constructor chaining* / §28.14 *Asynchronous
// and callback lifetimes* / §28.9 *Reference release*, v0.295).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp, `Emitter::emitChainCall`).
// A chain-target ctor takes its argument in `mk: () -> owned Res`. At the
// `Derived(x: int)` ctor body the lambda literal is evaluated as the `mk` slot:
// lowerLambda builds a fresh +1 closure cell (40 B) + env (16 B) + capture
// cells (4 B for the boxed `x`) and emits the call to `Base$ctor(ptr this,
// ptr closure)`. The non-chain `new T(args)` site already runs the #1073 /
// #2305 / #2305 release discipline — install the unwind-edge
// `closureArgUnwindCleanup`, call, then on the normal-return edge loop
// `emitClosureRelease` over each call-bounded closure arg
// (LlvmBackend_Exprs2.cpp:7945, LlvmBackend_Calls2.cpp:10780). The chain-call
// site never did: the `Derived$ctor` body that did the construction
// called the closure-release helper nowhere, and after `Base$ctor` returned
// the closure's only reference died with the ctor frame — Direct leak of
// 20000 byte(s) in 500 object(s) allocated from: bz$M.Derived$ctor (the
// closure cell, 40 B/iter), plus Indirect leaks of the env (16 B/iter) and
// the capture box (4 B/iter). The construct's own heap allocation was freed
// by the `new` site correctly; what leaked was the closure the chain
// argument evaluation produced and the chain call then consumed.
//
// FIX. `emitChainCall` now classifies each formal whose declared type is a
// function type AND whose source argument is a recognized +1 closure
// producer (`LambdaExpr` literal, static / instance method reference, or
// primitive-intrinsic method reference — `isPlusOneClosureProducer`), keyed
// on the chain target's parameter index via the same `paramToArg` map the
// §22.5 binding loop already maintains. Those closures are installed as the
// call-bounded release set BEFORE the chain call (`closureArgUnwindCleanup`
// for the throw / propagate edge, mutually exclusive with the post-call
// release), and on the normal-return edge `emitClosureRelease` is called
// for each. Mirrors lowerCall's method-call half and lowerNew's ctor-call
// half exactly — the chain-call lowering was the missing twin.
//
// Fails-without / passes-with: pre-fix the chain site allocates a fresh
// closure cell per iteration and never releases it, so the runAotChecked's
// LeakSanitizer net flips `r.ran` to false (-> run()==-999, distinct from
// any legal exit value). Post-fix every cell is freed exactly once: the
// closure cell + env (40+16 B) are released by `bz_closure_release` on the
// chain-call's normal-return edge, and the envdtor drops the env's
// contents (capture boxes). The `Res` production/disposal was already
// correct on both sides (the `mk()` invocation lives inside Base$ctor's
// `using var c := mk();` body), so the program reads `Res.live == 0` at
// exit either way; only the LSan leg distinguishes.
//
// Spec: §22.10 (Constructor chaining); §28.14 (call-bounded closure release);
// §28.9 (reference release); §28.3 (exception-path discharge). Spec revision
// v0.295.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value. The Res.live static (a process-global counter)
// confirms the `mk()` invocations ran the expected number of times and
// disposed everything they produced — on its own it cannot distinguish the
// pre-fix leak from the post-fix clean run (both reach 0), which is why the
// test leans on the LSan leg.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── T1 — the issue's exact repro (base-chain with lambda argument). The
// Derived ctor chains to Base via `base(lambda() => new Res(x))`; Base
// invokes `mk` exactly once. Pre-fix: 60 B/iter leaked (40+16+4) from
// bz$M.Derived$ctor. Post-fix: clean. Res.live == 0 either way; the LSan
// leg is the load-bearing assertion.
BEZEL_TEST(codegen_ctor_chain_lambda_arg_release_4077,
           base_chain_lambda_arg_no_leak_spec_22_10_28_14) {
    std::string src =
        "module M4077a; "
        "class Res "
        "  public static live: int := 0; "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; "
        "    Res.live := Res.live + 1; "
        "  end constructor; "
        "  destructor Res.live := Res.live - 1; end destructor; "
        "end Res; "
        "class Base "
        "  public constructor(mk: () -> owned Res) "
        "    using var c := mk(); "
        "  end constructor; "
        "end Base; "
        "class Derived: Base "
        "  public constructor(x: int) "
        "    base(lambda() => new Res(x)); "
        "  end constructor; "
        "end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      using var d := new Derived(9); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ── T2 — the SAME-class `this(...)` chain form (no inheritance). The
// issue's "what does NOT leak" grid rules out every other chain argument
// shape: a plain (non-chained) ctor is clean; a chain call with `int` or
// `owned Res` value arguments is clean; only the chain + LAMBDA combination
// triggers the gap. This case pins the `this(...)` arm — the same emitter
// (`emitChainCall`) handles both, so the fix must cover both spellings.
BEZEL_TEST(codegen_ctor_chain_lambda_arg_release_4077,
           this_chain_lambda_arg_no_leak_spec_22_10_28_14) {
    std::string src =
        "module M4077b; "
        "class Res "
        "  public static live: int := 0; "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; "
        "    Res.live := Res.live + 1; "
        "  end constructor; "
        "  destructor Res.live := Res.live - 1; end destructor; "
        "end Res; "
        "class Holder "
        "  public constructor(mk: () -> owned Res) "
        "    using var c := mk(); "
        "  end constructor; "
        "  public constructor(x: int) "
        "    this(lambda() => new Res(x)); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      using var h := new Holder(9); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ── T3 — the §22.10 INHERITANCE-CLAUSE spelling
// (`class D(): B(lambda() => new Res(x))`). The inheritance-clause base
// call is a distinct emitter path from the explicit `base(...)` arm (the
// `#617` substitution block in `emitChainCall`'s sibling
// `emitSpecializedImplicitBaseCtor`), but the same +1 closure release
// discipline must apply. Pre-fix: same leak as T1/T2. Post-fix: clean.
BEZEL_TEST(codegen_ctor_chain_lambda_arg_release_4077,
           inheritance_clause_lambda_arg_no_leak_spec_22_10_28_14) {
    std::string src =
        "module M4077c; "
        "class Res "
        "  public static live: int := 0; "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; "
        "    Res.live := Res.live + 1; "
        "  end constructor; "
        "  destructor Res.live := Res.live - 1; end destructor; "
        "end Res; "
        "class Base "
        "  public constructor(mk: () -> owned Res) "
        "    using var c := mk(); "
        "  end constructor; "
        "end Base; "
        "class Derived(x: int): Base(lambda() => new Res(x)) end Derived; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      using var d := new Derived(9); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// ── T4 — the CONTROL that pins the bound: the SAME lambda-argument
// constructor via the ordinary `new T(args)` site, which compiled and ran
// correctly BEFORE the fix (the lowerNew / lowerCall sites already enforced
// the release). Pre-fix and post-fix both pass; this case is here so the
// chain spelling and the `new` spelling are pinned together — they must
// not be allowed to drift apart again.
BEZEL_TEST(codegen_ctor_chain_lambda_arg_release_4077,
           direct_new_lambda_arg_control_spec_22_7_28_14) {
    std::string src =
        "module M4077d; "
        "class Res "
        "  public static live: int := 0; "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; "
        "    Res.live := Res.live + 1; "
        "  end constructor; "
        "  destructor Res.live := Res.live - 1; end destructor; "
        "end Res; "
        "class Holder "
        "  public constructor(mk: () -> owned Res) "
        "    using var c := mk(); "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      using var h := new Holder(lambda() => new Res(9)); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}
