// §19.6 (v0.242, #3052), verbatim: "when the scrutinee's static type is a
// `closed` class … and every leaf type in the closed hierarchy is matched by a
// type pattern (with or without binding), no `_` arm is required." §11.7:
// "Because `Shape` is `closed`, no class outside the declaring module may
// extend it. The compiler can therefore prove exhaustiveness of `case` matches
// over `Shape` without requiring a `_` arm, even across module boundaries."
//
// This is the §11.7 worked example verbatim in shape (Circle / Square /
// Triangle over a `closed class Shape`), reduced to `int` areas so the arm
// bodies stay one line. It must compile with NO diagnostic at all — in
// particular no §19.6 "case is not exhaustive" warning.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; closed class Shape public abstract method Area(): int; end Shape; sealed class Circle : Shape public override method Area(): int => 3; destructor end destructor; end Circle; sealed class Square : Shape public override method Area(): int => 4; destructor end destructor; end Square; sealed class Triangle : Shape public override method Area(): int => 5; destructor end destructor; end Triangle; class C public static method Describe(s: Shape): int => (case s of var c: Circle => 1, var q: Square => 2, var t: Triangle => 3); public static method Main(): int using var c := new Circle(); return Describe(c); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
