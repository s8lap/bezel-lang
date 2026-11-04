//! case: l2_4_0025
//! doc: stdlib
//! section: 2.4
//! title: Get's result is string?, so passing it to a string parameter is rejected
//! rule: The compiler enforces the distinction at every assignment, parameter pass,
//!   and return.
//! cites: 2.4, 7.1
//! expect: reject
//! diagnostic: nullable but the target type is non-nullable
//! issue: 8096
module Conf;

public class Program
  public static method Len(s: string): int
    return s.Length;
  end Len;

  public static method Main()
    Console.WriteLine(Len(Environment.Get("BZ_H3_A")));
  end Main;
end Program;
