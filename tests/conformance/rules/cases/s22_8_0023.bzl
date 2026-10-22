//! case: s22_8_0023
//! doc: spec
//! section: 22.8
//! title: a constructor may not pass a readonly array element of an owned field's instance as a
//!   ref argument
//! rule: "an *instance* `readonly` field is assignable only through an lvalue chain rooted at
//!   `this` ... so a constructor of the declaring type may **not** write another value's
//!   `readonly` storage held in a parameter, local, or field" (§22.8, v0.340); a `ref` or
//!   `out` argument is a write location and admits "no `readonly` field outside its
//!   per-instance assignable context" (§22.3). `this.Next` is another instance of `H`, so
//!   the chain `this.Next.A[0]` passes through a field of `this` but lands in the `readonly`
//!   storage of `Next`.
//! cites: 22.3
//! expect: reject
//! diagnostic: readonly
module Conf;

public static class Util
  public static method Bump(ref v: int)
    v := v + 1;
  end Bump;

  public static method Set(out v: int)
    v := 1;
  end Set;
end Util;

public class H
  public readonly A: [2]int;
  public owned Next: H?;

  public constructor()
    this.Next := new H();
    Util.Bump(ref this.Next!.A[0]);
  end constructor;
end H;
