// fix #3434 (codegen; language spec §4.7 "The unit type" / §21.7 "The `return`
// statement" / §32.5 "FFI-compatible types"; stdlib §6.9 `ChannelSender<T>.Send`,
// v0.283): #2963's arrow-body exemption was gated on the SPELLING of the return
// type, not on the type. `Emitter::arrowBodyLoweredAsUnit` read
//
//     return !declRet && dynamic_cast<const ast::CallExpr*>(body) &&
//            lastCallHandledWithoutValue_;
//
// and `!declRet` asks whether a return type was WRITTEN. So the identical
// program with `: ()` written out drew the note #2963 exists to suppress:
//
//   public static method Push(s: ChannelSender<int>): () => s.Send(42);
//     -> internal: return-value expression failed to lower to a value
//        (degraded to a zero/null return value) — #2491
//
// while deleting the `: ()` silenced it. §4.7 makes those one type — "`()` is
// the type with exactly one value, also written `()`. It is the return type of
// methods declared without an explicit return type" — and the FRONT END already
// agreed: §21.7 rejects `public static method Give(): () return C.Nothing();
// end Give;` with "this callable has no return type — `return` may not carry a
// value", because sema's `returnsValue` treats a declared `: ()` as having
// none. §32.5 says it outright for the declarations it governs: "An FFI
// declaration with no `:` clause produces a unit-returning method; an explicit
// `: ()` is also accepted and means the same thing."
//
// The fix asks sema's authority (`sema::ascribedReturnTypeProducesValue`, which
// `returnsValue` now also calls) instead of re-deciding it from syntax.
//
// THIS FILE IS THE DIAGNOSTIC ORACLE, and it has to be lit rather than gtest for
// the reason its #2963 sibling records: the AOT gtest harness discards captured
// compiler stderr on a SUCCESSFUL compile (`tests/support/aot_support.hpp`), so
// an "assert no warning" written there passes VACUOUSLY on any compiler, fixed
// or not. This program compiles and RUNS correctly on both sides of the fix —
// the note is the entire defect.
//
// FOUR INDEPENDENT ROUTES, all with the `: ()` ascription written, so this
// cannot pass by one arm having been special-cased — the first three mirror the
// #2963 sibling's three, and `Fwd` adds the GENERIC-SPECIALIZATION face, which
// is the predicate's second caller (`emitSpecializedMethodBody`) and a different
// #2491 site from the other three's:
//   * `s.Send(42)`           — `tryLowerChannelAndDomainCall`'s `Send` arm;
//   * `Console.WriteLine(s)` — `tryLowerConsoleCall`;
//   * `Bump()`               — ordinary method-symbol dispatch;
//   * `Fwd<T>`               — the generic specialization's own arrow-body face.
//
// FIXED SINCE, by issue 3497: the `C.Fwd<int>(c, 1)` call site used to emit the
// arrow body's call BOTH inline at the call site AND through `bz$M.C$Fwd`, so
// `Bump` ran twice per call. That was pre-existing and identical with and
// without the `: ()` ascription — unrelated to this file, which asserts only on
// diagnostics, and invisible to it. It is named here because this file's `Fwd`
// route is the exact program that carried it, and because the note this file
// pins is what its own fix removed, leaving that issue the sole record. The
// runtime oracle for the doubling now lives in
// tests/codegen/generic_arrow_unit_body_runs_once_3497_test.cpp.
//
// The NEGATIVE control lives in this directory as
// call_lowered_as_unit_declared_return_still_warns_2963.bzl (`: int`, which
// still warns) — and see `NullableUnit` below, the sharper one: a `()?`
// ascription is a DIFFERENT type ("an optional with two values, `null` and
// `()`", §4.7) whose arrow body really did degrade, so it must keep warning.
// That is what makes this a semantic gate rather than "any unit-ish spelling".
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Threading;

class Counter
  public _n: int := 0;
  public method Bump() _n := _n + 1; end Bump;
  public method Once(): () => Bump();
end Counter;

class C
  public static method Push(s: ChannelSender<int>): () => s.Send(42);
  public static method Say(s: string): () => Console.WriteLine(s);
  public static method Fwd<T>(c: Counter, x: T): () => c.Bump();
  public static method Main(): int
    using var ch := new Channel<int>();
    using var sender := ch.NewSender();
    C.Push(sender);
    C.Say("hi");
    using var c := new Counter();
    c.Once();
    C.Fwd<int>(c, 1);
    return ch.Count;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #2491
