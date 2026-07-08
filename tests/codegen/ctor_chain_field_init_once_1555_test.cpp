// Codegen — §22.8/§22.9 (v0.181): once-only field initializers under
// `this(...)` constructor chaining, and the once-only rule's completed
// exception-safety story. Issue #1555.
//
// §22.8: "Field initializers run exactly once per instance construction...
// They execute as step 2 of the constructor whose chaining position holds
// `base(...)` or the implicit `base()`... A constructor that chains via
// `this(...)` skips steps 0 and 2."
//
// §22.9 (v0.181) adds the matching unwind story for a `this(...)`-chaining
// constructor:
//   - "A throw during the chained call itself is the first case above...
//     the chained-to constructor's unwinding has by then disposed everything
//     its steps initialized — including derived fields... so there is
//     nothing further to dispose" (no EXTRA disposal at this level).
//   - "If the chaining constructor's body throws, the chained-to
//     construction completed and the instance is at that point fully
//     initialized: the runtime disposes it through the normal generated-
//     `Dispose()` path (§28.4) before the exception propagates."
//
// FINDINGS fixed by this issue (both in LlvmBackend_Setup.cpp):
//
//   [A] emitConstructorBody's ctor-cleanup block (the propagate target for a
//       non-generic constructor BODY throw) ran ONLY emitOwnedFieldDisposals +
//       emitBaseDisposeChain, unconditionally, whether or not the throwing
//       constructor was itself a `this(...)`-chaining one. Per §22.9's v0.181
//       text, a `this(...)`-chaining constructor's OWN body throw must ALSO
//       run the class's own user-written destructor (the "normal generated-
//       Dispose() path") — which the prior code never did. Fixed by branching
//       on `isThis` and, when true, calling this class's own CORE disposer
//       (disposerCoreFns_[cs->parent], not the public vtable-dispatched
//       Dispose — see the fix's comment for why a dynamic dispatch is unsafe
//       when this ctor is itself reached as a `base(...)` target from a
//       more-derived class).
//
//   [B] inlineCtorBody (the generic-instantiation ctor path, #585) installed
//       its cleanup block as `unwindTarget_` BEFORE running the leading
//       chain call, so a `this(...)`-chain-call-itself-throw ALSO ran the
//       CURRENT class's cleanup a second time over the same object — a
//       confirmed DOUBLE FREE (the chained-to ctor's own cleanup had already
//       freed the block; the caller's cleanup then freed it again). Fixed by
//       moving the cleanup-block install to AFTER the chain call (mirroring
//       emitConstructorBody's existing bare-propagate/derived-cleanup split),
//       and — mirroring finding [A] — running the class's own destructor via
//       specializedDisposerCoreFor (not disposerCoreFns_, whose template
//       layout doesn't match a non-int concrete T) when `isThis`.
//
// Tests below (destructor-count / exit-code observable, JIT):
//   T1 — field initializer runs exactly once under `this(...)` chaining.
//   T2 — the chain call itself throwing adds no extra disposal (§22.9
//        bullet 1) — non-generic.
//   T3 — a `this(...)`-chaining ctor's own body throw disposes the fully-
//        initialized instance's OWN destructor exactly once ([A]'s repro).
//   T4 — same as T3, but the field is reassigned before the throw: the
//        drop-on-overwrite (§28.7) dispose plus the final generated-Dispose()
//        dispose both fire (count == 2).
//   T5 — a DERIVED class's `this(...)`-chaining ctor body-throw disposes
//        both its own and the base's owned fields.
//   T6 — a GENERIC class's `this(...)`-chaining ctor body-throw runs the
//        class's own destructor ([A]/[B]'s generic analog).
//   T7 — a GENERIC class's `this(...)`-chain-call-itself-throw does not
//        double-free ([B]'s repro; was a crash pre-fix).
//   T8 — nested chaining: a `base(...)` call targets a constructor that is
//        ITSELF a `this(...)`-chaining constructor of the base class. When
//        the target's own body throws, only the BASE's destructor runs
//        (scoped to the base, §22.9 bullet 1) — the DERIVED class's
//        destructor must NOT run over its not-yet-constructed state.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ── T1: §22.8 — field initializer runs exactly once under this(...) chain ──
BEZEL_TEST(ctor_chain_init_once_1555, field_init_runs_once_under_this_chain) {
    // per §22.8 ("Field initializers run exactly once per instance
    // construction... A constructor that chains via `this(...)` skips
    // steps 0 and 2.")
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class R "
        "  public static _ctorCount: int := 0; "
        "  public static _dtorCount: int := 0; "
        "  public constructor() _ctorCount := _ctorCount + 1; end constructor; "
        "  destructor _dtorCount := _dtorCount + 1; end destructor; "
        "end R; "
        "class C "
        "  private owned _res: R := new R(); "
        "  public constructor() end constructor; "
        "  public constructor(x: int) this(); end constructor; "
        "end C; "
        "class Program public static method Main(): int "
        "  do "
        "    using var c := new C(1); "
        "    if R._ctorCount <> 1 then return 100 + R._ctorCount; end; "
        "    if R._dtorCount <> 0 then return 200 + R._dtorCount; end; "
        "  end; "
        "  if R._dtorCount <> 1 then return 300 + R._dtorCount; end; "
        "  return 0; "
        "end Main; end Program;"), 0);
}

// ── T2: §22.9 bullet 1 — chain-call-itself throw adds no extra disposal ────
BEZEL_TEST(ctor_chain_init_once_1555, chain_call_throw_no_extra_disposal) {
    // per §22.9 ("A throw during the chained call itself... the chained-to
    // constructor's unwinding has by then disposed everything its steps
    // initialized... so there is nothing further to dispose.")
    BEZEL_EXPECT_EQ(run(
        "module M; class AppError end AppError; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C "
        "  private owned _res: R := new R(); "
        "  public constructor() throw new AppError(); end constructor; "
        "  public constructor(x: int) this(); end constructor; "
        "end C; "
        "class Program public static method Main(): int "
        "  try do using var c := new C(1); end; "
        "  catch AppError => return R._n; end; "
        "  return -1; "
        "end Main; end Program;"), 1);
}

// ── T3: §22.9 (v0.181) — chaining ctor's own body throw runs OWN destructor ─
BEZEL_TEST(ctor_chain_init_once_1555, chaining_ctor_body_throw_runs_own_destructor) {
    // per §22.9 ("If the chaining constructor's body throws, the chained-to
    // construction completed and the instance is at that point fully
    // initialized: the runtime disposes it through the normal generated-
    // Dispose() path (§28.4) before the exception propagates.") Pre-fix [A]:
    // the ad hoc ctor-cleanup path never ran C's own destructor — _dtorRan
    // stayed 0.
    BEZEL_EXPECT_EQ(run(
        "module M; class AppError end AppError; "
        "class C "
        "  public static _dtorRan: int := 0; "
        "  public constructor() end constructor; "
        "  public constructor(x: int) this(); throw new AppError(); end constructor; "
        "  destructor _dtorRan := _dtorRan + 1; end destructor; "
        "end C; "
        "class Program public static method Main(): int "
        "  try do using var c := new C(1); end; "
        "  catch AppError => return C._dtorRan; end; "
        "  return -1; "
        "end Main; end Program;"), 1);
}

// ── T4: §22.9/§28.7 — reassignment-then-throw: drop-on-overwrite + final ───
BEZEL_TEST(ctor_chain_init_once_1555, chaining_ctor_body_reassign_then_throw) {
    // per §28.7 (drop-on-overwrite disposes the field-init value at the
    // reassignment) and §22.9 (the generated Dispose() then disposes the
    // reassigned value) — total dispose count == 2.
    BEZEL_EXPECT_EQ(run(
        "module M; class AppError end AppError; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C "
        "  private owned _res: R := new R(); "
        "  public constructor() end constructor; "
        "  public constructor(x: int) "
        "    this(); _res := new R(); throw new AppError(); "
        "  end constructor; "
        "end C; "
        "class Program public static method Main(): int "
        "  try do using var c := new C(1); end; "
        "  catch AppError => return R._n; end; "
        "  return -1; "
        "end Main; end Program;"), 2);
}

// ── T5: §22.9 — derived class chaining-body-throw disposes base + own ──────
BEZEL_TEST(ctor_chain_init_once_1555, derived_chaining_body_throw_disposes_base_and_own) {
    // per §22.9's "normal generated-Dispose() path" — the generated Dispose
    // walks the full base chain, so a derived this(...)-chaining ctor's body
    // throw must dispose both the derived AND base owned fields.
    BEZEL_EXPECT_EQ(run(
        "module M; class AppError end AppError; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class Base "
        "  private owned _baseRes: R := new R(); "
        "  public constructor() end constructor; "
        "end Base; "
        "class Derived : Base "
        "  private owned _derRes: R := new R(); "
        "  public constructor() base(); end constructor; "
        "  public constructor(x: int) this(); throw new AppError(); end constructor; "
        "end Derived; "
        "class Program public static method Main(): int "
        "  try do using var d := new Derived(1); end; "
        "  catch AppError => return R._n; end; "
        "  return -1; "
        "end Main; end Program;"), 2);
}

// ── T6: §22.9 generic analog — GENERIC class's own destructor runs ─────────
BEZEL_TEST(ctor_chain_init_once_1555, generic_chaining_ctor_body_throw_runs_own_destructor) {
    // per §14 (generic ctor inline, #585) / §22.9 — the inlineCtorBody path
    // ([B]) had the identical missing-destructor gap as the non-generic path.
    BEZEL_EXPECT_EQ(run(
        "module M; class AppError end AppError; "
        "class G<T> where T: class "
        "  public static _dtorRan: int := 0; "
        "  public constructor() end constructor; "
        "  public constructor(x: int) this(); throw new AppError(); end constructor; "
        "  destructor _dtorRan := _dtorRan + 1; end destructor; "
        "end G; "
        "class Cell end Cell; "
        "class Program public static method Main(): int "
        "  try do using var g := new G<Cell>(1); end; "
        "  catch AppError => return G<Cell>._dtorRan; end; "
        "  return -1; "
        "end Main; end Program;"), 1);
}

// ── T7: §22.9 generic analog — chain-call-itself-throw: no double free ─────
BEZEL_TEST(ctor_chain_init_once_1555, generic_chain_call_throw_no_double_free) {
    // per §14 (#585) / §22.9 bullet 1 — [B]'s repro: pre-fix this crashed
    // ("free(): double free detected in tcache") because inlineCtorBody
    // installed its cleanup as the active unwind target BEFORE the chain
    // call, so a chain-call-itself throw ran the (already-disposed) target's
    // cleanup a second time. rc must be 1 (single dispose), not a crash.
    BEZEL_EXPECT_EQ(run(
        "module M; class AppError end AppError; "
        "class R public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class G<T> where T: class "
        "  private owned _res: R := new R(); "
        "  public constructor() throw new AppError(); end constructor; "
        "  public constructor(x: int) this(); end constructor; "
        "end G; "
        "class Cell end Cell; "
        "class Program public static method Main(): int "
        "  try do using var g := new G<Cell>(1); end; "
        "  catch AppError => return R._n; end; "
        "  return -1; "
        "end Main; end Program;"), 1);
}

// ── T8: §22.9 bullet 1 — nested base(...)-target that is itself this(...)- ──
//        chaining: disposal stays scoped to the BASE, not the derived class.
BEZEL_TEST(ctor_chain_init_once_1555, nested_base_target_this_chain_scoped_to_base) {
    // per §22.9 bullet 1: "An exception during the base or chained
    // constructor call (step 1) leaves no derived state to clean up — the
    // base ctor's own exception-safety logic disposes whatever it had
    // constructed of the base subobject." Here Derived's `base(y)` targets
    // Base(int), which is ITSELF a this(...)-chaining constructor whose body
    // throws. Only Base's own destructor may run (scoped statically to
    // Base, per the fix's use of the CORE disposer rather than a dynamic
    // vtable dispatch); Derived's destructor must NOT run — Derived's own
    // fields/state were never constructed (the throw is Derived's step 1).
    BEZEL_EXPECT_EQ(run(
        "module M; class AppError end AppError; "
        "class Base "
        "  public static _baseDtorRan: int := 0; "
        "  public constructor() end constructor; "
        "  public constructor(x: int) this(); throw new AppError(); end constructor; "
        "  destructor _baseDtorRan := _baseDtorRan + 1; end destructor; "
        "end Base; "
        "class Derived : Base "
        "  public static _derivedDtorRan: int := 0; "
        "  public constructor(y: int) base(y); end constructor; "
        "  destructor _derivedDtorRan := _derivedDtorRan + 1; end destructor; "
        "end Derived; "
        "class Program public static method Main(): int "
        "  try do using var d := new Derived(1); end; "
        "  catch AppError => do "
        "    if Base._baseDtorRan <> 1 then return 100 + Base._baseDtorRan; end; "
        "    if Derived._derivedDtorRan <> 0 "
        "      then return 200 + Derived._derivedDtorRan; end; "
        "    return 0; "
        "  end "
        "  end; "
        "  return -1; "
        "end Main; end Program;"), 0);
}
