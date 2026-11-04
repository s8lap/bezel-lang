//! case: s22_15_0032
//! doc: spec
//! section: 22.15
//! title: a lambda body inside a pure body is not checked under the pure rules
//! rule: A lambda *body* defined inside a pure body is not checked under these
//!   rules: it is a separate function that provably cannot run during the
//!   pure call
//! cites: 22.15
//! expect: run
//! issue: 7453
//! output: formed
//!   1
//!   11
module Conf;

public class Ops
  public static method Log(v: int): int
    Console.WriteLine(v);
    return v;
  end Log;

  public static pure method Make(x: int): (int) -> int
    return lambda(y: int) => Log(y) + x;
  end Make;
end Ops;

public class Program
  public static method Main()
    var f := Ops.Make(10);
    Console.WriteLine("formed");
    Console.WriteLine(f(1));
  end Main;
end Program;
