//! case: s4_2_0008
//! doc: spec
//! section: 4.2
//! title: an out-of-int-range literal is rejected at a generic parameter explicitly bound to double
//! rule: In an untyped context (var x := 3_000_000_000;), and at a non-integer
//!   target (var d: double := 3_000_000_000;), the literal is a compile error
//!   directing to a suffix (3_000_000_000L, which then converts by §5.1).
//! cites: 4.2, 17.7, 14.5
//! expect: reject
//! diagnostic: add a suffix
module Conf;

public static class Util
  public static method Id<T>(a: T): T => a;
end Util;

public class Program
  public static method Main()
    Console.WriteLine(Util.Id<double>(3000000000));
  end Main;
end Program;
