// #2133 (§21.7 / §11.7 / §19.6): the GENERIC-leaf shape of the sealed-hierarchy
// statement-case totality grant — matching the issue's example: a determined
// `Ok<int>` / `Err<int>` leaf set over scrutinee `Result<int>`.
//
// `Result` is `sealed abstract` (§11.7), so the hierarchy is closed across
// module boundaries. §19.6's determination rule makes `Ok<int>` and `Err<int>`
// the sole inhabiting instantiations at the scrutinee's static type `Result<int>`
// — the canonical "determined" exhaustive shape. Every arm returns; there is no
// trailing return/throw after the `case`. Per §21.7 (closed-value-space bullet:
// sealed-hierarchy, §11.7), such an exhaustive statement-`case` whose arms all
// diverge is TOTAL — no trailing exit is required.
//
// Without the fix, §21.7 fired "does not return a value on every path"
// (WRONG-without); with the fix, the determined-instantiation leaf coverage
// closes totality and no diagnostic fires (CORRECT-with).
//
// Cite spec §21.7, §11.7, §19.6 (v0.226).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

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

// NODIAG-NOT: error
// NODIAG-NOT: §
