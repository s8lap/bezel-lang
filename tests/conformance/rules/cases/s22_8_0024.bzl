//! case: s22_8_0024
//! doc: spec
//! section: 22.8
//! title: a lambda in a constructor may not bind its own readonly array element to a ref
//!   parameter
//! rule: "The window is lexical and does not include a nested function body ... an assignment to
//!   a `readonly` field inside a lambda ... is a compile error wherever the lambda stands,
//!   and so is binding the field to a `ref` or `out` parameter there" (§22.8, v0.340). The
//!   twin `Util.Bump(ref this.A[0])` directly in the constructor body is the accepting case
//!   s22_8_0025.
//! cites: 22.3, 27.3
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

  public constructor()
    var f := lambda() => do Util.Bump(ref this.A[0]); end;
    f();
  end constructor;
end H;
