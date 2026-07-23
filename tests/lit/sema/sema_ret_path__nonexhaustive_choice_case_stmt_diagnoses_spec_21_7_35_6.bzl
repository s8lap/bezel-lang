// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2129 (§21.7 / §35.6): the choice case-statement totality grant is gated on
// EXHAUSTIVENESS. Here the choice `R` has three cases but the case-statement
// covers only two (`Pending` is missing). Every covered arm returns, but a
// `Pending` value matches no arm and — in STATEMENT position — falls through
// with no action (§19.6, NOT `MatchFailureException`), reaching the method end
// with no return. §21.7 must still fire. This pins that the fix did not blanket
// every choice case-statement: only a PROVABLY exhaustive one is total.
//
// Cite spec §21.7, §19.6, §35.6 (v0.217).

module M;
choice R Ok(v: int), Error(m: int), Pending; end R;
class C
  public method Classify(r: R): int
    case r of
      Ok(var v)    => return v;,
      Error(var m) => return m;
    end;
  end Classify;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Classify` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
