//! case: s22_6_0006
//! doc: spec
//! section: 22.6
//! title: example 3 -- exact on one argument, conversion on the other, is unambiguous
//! rule: "A candidate is unambiguously better than another when it is
//!   better-or-equal on every argument position and strictly better on at
//!   least one."
//! cites: 22.6
//! expect: run
//! stdout: int,int
module Conf;

public class Pair
  public constructor()
  end constructor;

  public method Apply(x: int, y: long): string => "int,long";
  public method Apply(x: int, y: int): string => "int,int";
end Pair;

public class Program
  public static method Main()
    var p := new Pair();
    var a: int := 5;
    var b: int := 9;
    Console.WriteLine(p.Apply(a, b));
  end Main;
end Program;
