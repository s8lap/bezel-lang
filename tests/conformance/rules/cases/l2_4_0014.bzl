//! case: l2_4_0014
//! doc: stdlib
//! section: 2.4
//! title: Get returns the same value when passed straight to a nullable parameter
//! rule: public static method Get(name: string): string?;    // null when the variable is absent
//! expect: run
//! output: 5
//!   -1
//! issue: 7950
//! notes: Argument shape: the unbound result of Get reaches a string? parameter.
module Conf;

public class Program
  private static method Len(s: string?): int
    if s = null then return -1; end if;
    return s.Length;
  end Len;

  public static method Main()
    Environment.Set("BZ_H2_GET_B", "value");
    Console.WriteLine(Len(Environment.Get("BZ_H2_GET_B")));
    Console.WriteLine(Len(Environment.Get("BZ_H2_GET_ABSENT_91c2")));
  end Main;
end Program;
