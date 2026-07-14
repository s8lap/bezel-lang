// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #1816 (s9) — §19.6 v0.196 / §21.7: the generic-leaf shape of the totality
// invariant, matching the issue's example (a determined `Ok<int>` / `Err<int>`
// over scrutinee `Result<int>`).
//
// §19.6's determination rule (v0.196) makes `Ok<int>` and `Err<int>` the sole
// inhabiting instantiations at `Result<int>`, so this is the canonical
// "determined" exhaustive shape. Every arm returns; there is no trailing
// return/throw after the `case`. §21.7 must still require one — totality does
// NOT key on the §19.6 proof (v0.196 N6, "Totality must not key on the proof").
//
// Robustness: a statement-position `case` never requires an exhaustiveness proof
// to *compile* (an unmatched value falls through with no action, §19.6), so this
// program compiles today regardless of how far §19.6 generic-leaf determination
// (sibling lanes s1–s8 of #1816) has landed — and the §21.7 return-coverage
// analysis fires purely on the absence of a syntactic `_` catch-all, not on the
// proof. Once generic determination lands, this becomes a *proven*-exhaustive
// case and the pin only tightens. The test FAILS if the §19.6 proof is ever
// wired into totality.
//
// Cite spec §19.6, §21.7 (v0.196).

module M;
sealed abstract class Result<T>
  public abstract method IsOk(): bool;
end Result;
sealed class Ok<T> : Result<T>
  public V: T;
  public constructor(v: T) V := v; end constructor;
  public override method IsOk(): bool => true;
  destructor end destructor;
end Ok;
sealed class Err<T> : Result<T>
  public E: string;
  public constructor(e: string) E := e; end constructor;
  public override method IsOk(): bool => false;
  destructor end destructor;
end Err;
class C
  public method Classify(r: Result<int>): int
    case r of
      var o: Ok<int> => return 1;,
      var e: Err<int> => return 2;
    end;
  end Classify;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Classify` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)
