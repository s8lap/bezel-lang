// fix #2962 — ACCEPT-CONTROL for spec §17.4's unit arm (v0.241).
//
// §17.4 draws the boundary in the same breath as the rejection: "`()?` remains
// a well-formed type (§4.7); the rejection is of the operator form, not of the
// type." §4.7: "Wrapping the unit type with `?` (yielding `()?`) is permitted by
// the grammar and produces an optional with two values, `null` and `()`."
//
// So the type must keep compiling where it is WRITTEN as a type, while
// `x as ()` does not. An over-broad fix that rejected `Kind::Unit` wherever it
// appeared would still pass the rejection test next door; only this file
// catches it. Both of §4.7's two values are exercised, and both null tests, so
// the control cannot be satisfied by a `()?` that merely parses.
//
// SCOPE, measured rather than assumed: the LOCAL positions below are the ones
// that work today. `()?` at a SIGNATURE position does not — a parameter of type
// `()?` CRASHES the compiler (SIGILL) and a `()?` return type is rejected as
// "no return type" (§21.7). Both are pre-existing and orthogonal to #2962 (no
// `as` is involved); filed as **#3095**. When that lands, add a `()?` parameter
// and a `()?`-returning method here — this control is deliberately narrower
// than §4.7 is, and the gap is tracked, not forgotten.
//
// RUN: %bezelc -c -o %t.o %s

module M;

class C
  public static method Main(): int
    var u: ()? := null;
    var v: ()? := ();
    var code := 0;
    if u = null then code := code + 1; end;
    if u <> null then code := code + 8; end;
    if v <> null then code := code + 2; end;
    if v = null then code := code + 16; end;
    return code;
  end Main;
end C;
