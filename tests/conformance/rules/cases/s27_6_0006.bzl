//! case: s27_6_0006
//! doc: spec
//! section: 27.6
//! title: an exact-match overload is preferred over a converting one when forming a reference
//! rule: Among unifying candidates, one whose parameter types and return type
//!   match the target's exactly is preferred; candidates that unify only
//!   through conversion are co-equal and lose to the exact candidate.
//! cites: 27.6
//! expect: run
//! stdout: int
module Conf;

public class Ops
  public static method Twice(x: int): int
    Console.WriteLine("int");
    return x * 2;
  end Twice;

  public static method Twice(x: long): long
    Console.WriteLine("long");
    return x * 2;
  end Twice;
end Ops;

public class Program
  public static method Main()
    var f: (int) -> int := Ops.Twice;
    Console.WriteLine($"{f(5)}");
  end Main;
end Program;
