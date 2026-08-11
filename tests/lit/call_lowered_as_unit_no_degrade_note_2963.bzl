// fix #2963 (codegen; language spec §21.7 "The return statement" / §4.7 the unit
// type; stdlib §6.9 `ChannelSender<T>.Send`, v0.240): `Emitter::lowerCall`
// returned `nullptr` for TWO categorically different outcomes — "no arm lowered
// this call" and "an arm lowered it and the result is UNIT" — and nothing
// downstream could tell them apart.  The visible cost was the #2491 return-degrade
// note firing on a program with nothing wrong with it:
//
//   public static method Push(s: ChannelSender<int>) => s.Send(42);
//     -> internal: return-value expression failed to lower to a value
//        (degraded to a zero/null return value) — #2491
//
// `Send` lowered (it emits `bz_channel_sender_send`; the item really is
// buffered), the callable declares no return type, and §21.7 states the rule
// that makes that legal — "this callable has no return type — `return` may not
// carry a value".  Nothing degraded, so nothing may say it did: #2434/#2491/#2775
// exist to CREATE a signal for silent codegen degrades, and a copy of that signal
// on a working program erodes the only evidence the next reader has.
//
// THIS FILE IS THE DIAGNOSTIC ORACLE.  It has to be lit rather than gtest: the
// AOT gtest harness populates `JitResult::diagnostics` only on a compile REJECT,
// so an "assert no warning" written there passes VACUOUSLY on any compiler,
// fixed or not.  The runtime half — that each of these calls really did lower —
// is pinned separately in
// tests/codegen/call_lowered_as_unit_vs_declined_2963_test.cpp.
//
// THREE INDEPENDENT ROUTES, so this cannot pass by one arm having been
// special-cased:
//   * `s.Send(42)`          — `tryLowerChannelAndDomainCall`'s `Send` arm, which
//                             emits and then `return nullptr` (the filed repro);
//   * `Console.WriteLine(s)`— `if (tryLowerConsoleCall(call)) return nullptr;
//                             // unit result`, a bool-returning helper several
//                             thousand lines away sharing no code with it;
//   * `Bump()`              — no `try*Call` arm at all: the ordinary
//                             method-symbol dispatch, on a user method that
//                             declares no return type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// internal: / #2491 diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System.Threading;

class Counter
  public _n: int := 0;
  public method Bump() _n := _n + 1; end Bump;
  public method Twice() => Bump();       // unit arrow over ordinary dispatch
end Counter;

class C
  public static method Push(s: ChannelSender<int>) => s.Send(42);   // §6.9
  public static method Say(s: string) => Console.WriteLine(s);
  public static method Main(): int
    using var ch := new Channel<int>();
    using var sender := ch.NewSender();
    C.Push(sender);
    C.Say("hi");
    using var c := new Counter();
    c.Twice();
    return ch.Count;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: internal:
// NODIAG-NOT: #2491
