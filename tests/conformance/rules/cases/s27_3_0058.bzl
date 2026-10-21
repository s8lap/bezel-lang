//! case: s27_3_0058
//! doc: spec
//! section: 27.3
//! title: a procedure lambda selects the () -> () constructor and an expression lambda the () -> int one
//! rule: "between overloads taking `() -> int` and `() -> ()` only the procedure slot is applicable" ([§27.3]); the expression-bodied lambda `lambda() => 3` is value-returning and takes the `() -> int` slot.
//! cites: 27.3, 22.6
//! expect: run
//! output: unit
//!   int
//! issue: 6243
module Conf;

public class K
  public _s: string;
  public constructor(f: () -> int) _s := "int"; end constructor;
  public constructor(f: () -> ()) _s := "unit"; end constructor;
end K;

public class Program
  public static method Main()
    var a := new K(lambda() => do Console.WriteLine("x"); end);
    var b := new K(lambda() => 3);
    Console.WriteLine(a._s);
    Console.WriteLine(b._s);
  end Main;
end Program;
