//! case: s14_5_0003
//! doc: spec
//! section: 14.5
//! title: argument-driven unification infers a type parameter from a bare-parameter position
//! rule: A parameter type that is a bare type parameter X binds X to the
//!   argument's static type (§14.5, phase 1).
//! cites: 14.5
//! expect: run
//! stdout: 3
module Conf;

public static class Util
  public static method Same<T>(a: T, b: T): T => a;
end Util;

public class Program
  public static method Main()
    var x := Util.Same(3, 5);
    Console.WriteLine(x);
  end Main;
end Program;
