// §28.3 *Borrow lifetime rules* (v0.286), issue #3743 — the ACCEPT boundary of
// the frame-chain widening. Byte-for-byte the #3743 reject row with `f` moved
// INTO the enclosing lambda's own body, beside `r`: the two now die at the same
// block end, so no call of `f` can be sequenced after `r`'s disposal and §28.3's
// "strictly encloses" premise is false.
//
// This is the row that pins the fix did NOT become "any store to a target
// outside the checked lambda body is rejected" — the failure mode #3707's own
// first draft had, and the reason the frame-chain comparison decides membership
// POSITIVELY (a hit in the sealed chain for BOTH operands) rather than reading a
// miss as a verdict.
//
// RUN: %bezelc -c %s

module M;
import System;
class R
  private v: int := 5;
  public method V(): int
    return this.v;
  end V;
end R;
class C
  public static method Run(): int
    var outer := lambda(): int => do
      using var r := new R();
      var f := lambda(): int => 0;
      var mk := lambda(): int => do
        f := lambda(): int => r.V();
        return 0;
      end;
      var t := mk();
      return t + f();
    end;
    return outer();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;
