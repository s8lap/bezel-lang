// fix #4171 (stdlib §8.1, v0.298): `Stopwatch.StartNew()`-typed receivers hit
// a spurious `internal: codegen cannot dispatch member 'Stop'/'Start'/
// 'Reset'` warning even though the call IS correctly lowered — the runtime
// behaviour was always correct (Stop() really stops the watch), only the
// diagnostic was wrong.
//
// Root cause: `LlvmBackend_Calls2.cpp:4964`'s `if (auto* v =
// tryLowerBuiltinClassCall(call)) return v;` cannot distinguish "nothing
// dispatched this call" from "an arm dispatched it and the result is unit" —
// the same #2963 ambiguity its sibling `ftRecv`-based Error check (a few
// dozen lines above this test's own CHECK target) already resolves via
// `emittedSideEffectSince`. The Shape 1/2 Warning further down the SAME
// end-of-chain fallthrough never got that treatment, so a void-returning
// dispatch that DID emit real code (`sw.Stop()`'s dedicated instance-method
// block, `LlvmBackend_Calls.cpp` ~13121, emits `bz_stopwatch_stop` and
// correctly returns nullptr for its unit result) still warned "cannot
// dispatch member 'Stop'". `Stopwatch.StartNew()`'s receiver types with a
// non-null `declaration` (`ExprTypecheck_Calls.cpp` ~8160/~21011 —
// `builtinClassSym("Stopwatch")`), which is what made the warning's
// `isConcreteType` check fire; `new Stopwatch()`'s receiver resolves through
// `TypeResolution_Names.cpp`'s "nullary stdlib class" list with a NULL
// declaration, so the constructor form never triggered this bug (and its
// accept-control below proves that stayed true).
//
// The fix trusts `emittedSideEffectSince` only when the receiver expression
// is a bare `ast::IdentifierExpr` (a local/parameter read — provably one
// non-side-effecting `load`, the issue's own repro shape). A receiver that
// is itself a call (`Stopwatch.StartNew().Bogus();`, no intermediate
// binding) keeps the pre-existing always-warn behaviour, because evaluating
// THAT receiver emits real side effects of its own (`bz_stopwatch_alloc` /
// `bz_stopwatch_start`) before the member name is even compared, which would
// otherwise make the footprint check misread "receiver evaluated" as "member
// dispatched" and silently drop a genuine #2963 diagnostic. Measured on the
// unpatched tree: `Stopwatch.StartNew().TotallyBogus4171C();` as a bare
// statement warns correctly both before AND after this fix — see the second
// must-still-warn control below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M;

import System.Diagnostics;

class Widget4171
  public constructor() end constructor;
end Widget4171;

class C
  public static method Main(): int
    // The pinned shape (fix #4171): Start/Stop/Reset on a
    // `Stopwatch.StartNew()`-typed LOCAL VARIABLE — the issue's own repro —
    // must be SILENT. The call really is dispatched (§8.1: Stop() sets
    // IsRunning = false; diagnostics_646_test.cpp's stopwatch_startnew_stop
    // already pins the runtime behaviour, this file pins the diagnostic).
    using var sw := Stopwatch.StartNew();
    sw.Start();
    sw.Stop();
    sw.Reset();

    // Accept-control: the `new Stopwatch()` constructor-typed receiver was
    // never affected either way (its TypeRef's `declaration` is null, so
    // `isConcreteType`'s Shape 1 never fired for it) — proving the fix did
    // not accidentally rely on, or regress, the constructor form's own
    // (always-silent) shape.
    using var sw2 := new Stopwatch();
    sw2.Start();
    sw2.Stop();
    sw2.Reset();

    // Must-still-warn control 1: a genuinely undispatchable member on an
    // ordinary Object-typed receiver — the original #2477/#2963 shape,
    // entirely unrelated to Stopwatch — must be completely unaffected.
    var w: Object := new Widget4171();
    w.TotallyBogus4171B();

    // Must-still-warn control 2: a genuinely undispatchable member reached
    // through the SAME `Stopwatch.StartNew()` receiver TYPE, but via a
    // receiver EXPRESSION the fix deliberately does not trust (the receiver
    // is itself a call, not a plain local read — see the file header).
    // Proves the fix is scoped to the exact receiver shape it was measured
    // against, not a blanket exemption for every Stopwatch call.
    Stopwatch.StartNew().TotallyBogus4171C();

    return 0;
  end Main;
end C;

// CHECK-NOT: cannot dispatch member 'Start'
// CHECK-NOT: cannot dispatch member 'Stop'
// CHECK-NOT: cannot dispatch member 'Reset'
// CHECK: internal: codegen cannot dispatch member 'TotallyBogus4171B' on receiver of type 'Object'
// CHECK: internal: codegen cannot dispatch member 'TotallyBogus4171C' on receiver of type 'Stopwatch'
