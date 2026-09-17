//! case: s14_5_0005
//! doc: spec
//! section: 14.5
//! title: an explicit type argument resolves an agreement failure
//! rule: A conflicting-inference failure is remedied with explicit type
//!   arguments: with an explicit list, the parameter type is fixed and each
//!   argument converts at the ordinary argument-conversion site (§14.5,
//!   *Agreement*).
//! cites: 14.5
//! expect: run
//! stdout: 1
module Conf;

public static class Util
  public static method Same<T>(a: T, b: T): T => a;
end Util;

public class Program
  public static method Main()
    var x := Util.Same<long>(1, 2L);
    Console.WriteLine(x);
  end Main;
end Program;
