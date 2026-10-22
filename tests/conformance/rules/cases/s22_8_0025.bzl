//! case: s22_8_0025
//! doc: spec
//! section: 22.8
//! title: a constructor may bind its own readonly array elements to ref and out parameters
//! rule: "an *instance* `readonly` field is assignable only through an lvalue chain rooted at
//!   `this` — an explicit `this.F := e` or the bare in-scope spelling that resolves to
//!   `this`'s storage" (§22.8, v0.340); `ref this.A[0]`, `out this.A[1]` and the bare `ref
//!   A[1]` are chains rooted at `this`, so each binding is admitted in the constructor body.
//! cites: 22.3
//! expect: run
//! stdout: 3
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
    Util.Bump(ref this.A[0]);
    Util.Set(out this.A[1]);
    Util.Bump(ref A[1]);
  end constructor;
end H;

public class Program
  public static method Main()
    var h := new H();
    Console.WriteLine(h.A[0] + h.A[1]);
  end Main;
end Program;
