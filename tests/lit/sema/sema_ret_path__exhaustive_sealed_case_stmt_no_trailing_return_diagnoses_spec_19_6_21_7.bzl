// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #1816 (s9) — §19.6 v0.196 / §21.7: an exhaustiveness PROOF must NOT satisfy
// TOTALITY.
//
// §19.6 (v0.196) proves this `case` statement exhaustive: `Shape` is a
// `sealed abstract` closed hierarchy (§11.7) whose every leaf — `Circle`,
// `Square` — is covered by a type-test arm, so in *expression* position it
// draws no non-exhaustiveness warning (that companion accept is exercised by
// tests/sema/case_exhaustiveness_test.cpp). Every arm here ends in `return`.
//
// Yet the method has NO trailing return/throw after the `case`, so §21.7's
// "must return on every exit path" rule must still fire: the §19.6 proof is
// warning-level and totality does NOT key on it (v0.196 ruling, N6 —
// "Totality must not key on the proof"). If it did, an exhaustive-looking case
// could suppress a required return and silently change program meaning at the
// runtime no-match edge, which the ruling forbids.
//
// The §21.7 return-coverage analysis (ExprTypecheck.cpp `stmtAlwaysExits`)
// treats a statement `case` as always-exiting ONLY when a syntactic unguarded
// `_` arm covers the no-match fall-through AND every arm exits — it never
// consults the §19.6 sealed/enum/bool exhaustiveness proof. This test FAILS if
// a future refactor wires that proof into totality (the error would vanish).
//
// Cite spec §19.6, §21.7 (v0.196).

module M;
sealed abstract class Shape
  public abstract method Tag(): int;
end Shape;
sealed class Circle : Shape
  public override method Tag(): int => 1;
  destructor end destructor;
end Circle;
sealed class Square : Shape
  public override method Tag(): int => 2;
  destructor end destructor;
end Square;
class C
  public method Classify(s: Shape): int
    case s of
      var c: Circle => return 1;,
      var q: Square => return 2;
    end;
  end Classify;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Classify` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
