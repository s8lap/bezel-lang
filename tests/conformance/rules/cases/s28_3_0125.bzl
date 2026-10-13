//! case: s28_3_0125
//! doc: spec
//! section: 28.3
//! title: reassigning a local disposed on one path frees that value without disposing it again
//! rule: An **explicitly-disposed** LHS requires an owned RHS too and displaces
//!   a **logically disposed** value: the store is that value's release point
//!   — §28.2's compiler-inserted free, the `Dispose()` a no-op on the set flag
//!   and then the reclamation of the block ... A **maybe-consumed** LHS ...
//!   disposes the displaced value on the paths that still own it, frees the
//!   logically disposed value on the explicitly-disposed paths
//! cites: 28.3, 28.2, 28.7
//! expect: run
//! output: closed 1
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
  public static method Run(c: bool)
    var x := new Res(1);
    if c then
      x.Dispose();
    end;
    x := new Res(2);
    Console.WriteLine($"holding {x.N}");
  end Run;

  public static method Main()
    Run(true);
    Run(false);
  end Main;
end Demo;
