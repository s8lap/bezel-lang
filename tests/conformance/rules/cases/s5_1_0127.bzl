//! case: s5_1_0127
//! doc: spec
//! section: 5.1
//! title: an int constant that does not fit byte is rejected at an explicitly bound generic parameter
//! rule: A constant whose value does not fit the target's range is a compile error
//!   at the conversion site — never a silent truncation.
//! cites: 5.1, 14.5
//! expect: reject
//! diagnostic: does not fit the target's range
module Conf;

public static class Util
  public static method Id<T>(a: T): T => a;
end Util;

public class Program
  public static method Main()
    const K: int := 300;
    var b: byte := Util.Id<byte>(K);
    Console.WriteLine(b);
  end Main;
end Program;
