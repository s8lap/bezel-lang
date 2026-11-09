// fix #7667 (§23.4 Indexers / §26.5 Resolution, v0.355): the NEGATIVE
// control for the accept row in this directory
// (generic_extension_this_indexer_nongeneric_no_dropped_note_7667.bzl) — and
// the reason that row's suppression is not over-broad.
//
// WHAT IS STILL ANNOUNCED. The #2833 announcement fires when the receiver
// cannot be recovered "neither from the enclosing function nor from the
// resolved indexer". Inside the extension method's OWN body on a built-in
// collection, the receiver IS recoverable from the enclosing function (the
// enclosing function IS the extension method, whose receiver's type is the
// extension's target type), so the announcement's premise is false there and
// #7667 suppresses it.
//
// Inside a NESTED CLOSURE body — a lambda, and equally an `async` body — the
// enclosing function is the CLOSURE, not the extension method, and there is no
// indexer PropertySymbol to recover the receiver from either: a built-in
// collection declares no indexer member (#2530 attaches one only for a
// user-declared indexer). THERE the receiver genuinely cannot be recovered,
// so the announcement must STILL fire — this is the #2833 family it was
// written for, and it stays armed.
//
// The row therefore pins BOTH halves at once: the announcement is present
// (the `CHECK` below) and the program's value is right (11 — the lambda's
// `this[0]` reads the element, so the write stores 1 + 10).
//
// RUN: %bezelc -c %s 2>&1 | FileCheck %s
// RUN: %bezelc -o %t.exe %s && %t.exe | FileCheck --check-prefix=OUT %s
module M;
extension Array<int>
  public method BumpViaLambda()
    var f := lambda(): int => this[0];
    this[0] := f() + 10;
  end BumpViaLambda;
end;

public static class Program
  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    xs.BumpViaLambda();
    Console.WriteLine(xs[0]);
    return 0;
  end Main;
end;

// CHECK: warning: internal: this `this[i]` indexer access was DROPPED
// CHECK-SAME: #2833
// OUT: 11
