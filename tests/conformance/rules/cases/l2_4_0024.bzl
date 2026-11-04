//! case: l2_4_0024
//! doc: stdlib
//! section: 2.4
//! title: Get's result is string?, so returning it from a string method is rejected
//! rule: The compiler enforces the distinction at every assignment, parameter pass,
//!   and return.
//! cites: 2.4, 7.1
//! expect: reject
//! diagnostic: nullable but the target type is non-nullable
//! issue: 8096
module Conf;

public class Program
  public static method Pick(): string
    return Environment.Get("BZ_H3_A");
  end Pick;

  public static method Main()
    Console.WriteLine(Pick());
  end Main;
end Program;
