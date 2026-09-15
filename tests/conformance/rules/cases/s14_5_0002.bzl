//! case: s14_5_0002
//! doc: spec
//! section: 14.5
//! title: a full explicit type-argument list binds every type parameter
//! rule: An explicit type-argument list binds the declared type parameters in
//!   declaration order; when a full explicit list is supplied, no inference
//!   runs (§14.5).
//! cites: 14.5
//! expect: run
//! stdout: 1
module Conf;

public static class Util
  public static method Pick<T, U>(a: T, b: U): T => a;
end Util;

public class Program
  public static method Main()
    var x := Util.Pick<int, string>(1, "s");
    Console.WriteLine(x);
  end Main;
end Program;
