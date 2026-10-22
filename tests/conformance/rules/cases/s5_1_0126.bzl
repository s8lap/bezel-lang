//! case: s5_1_0126
//! doc: spec
//! section: 5.1
//! title: an int constant narrows to byte at an explicitly bound generic parameter
//! rule: An expression of integer type that is a compile-time constant (§17.14)
//!   implicitly converts to any integer type whose range contains its value.
//! cites: 5.1, 14.5
//! expect: run
//! stdout: 200
module Conf;

public static class Util
  public static method Id<T>(a: T): T => a;
end Util;

public class Program
  public static method Main()
    const K: int := 200;
    var b: byte := Util.Id<byte>(K);
    Console.WriteLine(b);
  end Main;
end Program;
