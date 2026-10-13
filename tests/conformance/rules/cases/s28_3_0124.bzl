//! case: s28_3_0124
//! doc: spec
//! section: 28.3
//! title: a maybe-consumed owned local is reassignable and displaces only on the paths that own it
//! rule: A **maybe-consumed** LHS (K1 state 4) is an owned-state LHS whose
//!   displaced value exists on exactly the paths that did not consume it: the
//!   store requires an owned RHS, disposes the displaced value on the paths
//!   that still own it ... and displaces nothing on the moved-from ones ...
//!   and leaves the binding owned and readable.
//! cites: 28.3, 28.7
//! expect: run
//! output: taken 1
//!   closed 1
//!   holding 2
//!   closed 2
//!   closed 1
//!   holding 2
//!   closed 2
//! issue: 5810
module Conf;

public class Res
  public property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  destructor
    Console.WriteLine($"closed {N}");
  end destructor;
end Res;

public class Demo
  public static method Take(owned r: Res)
    Console.WriteLine($"taken {r.N}");
  end Take;

  public static method Run(c: bool)
    var x := new Res(1);
    if c then
      Take(move x);
    end;
    x := new Res(2);
    Console.WriteLine($"holding {x.N}");
  end Run;

  public static method Main()
    Run(true);
    Run(false);
  end Main;
end Demo;
