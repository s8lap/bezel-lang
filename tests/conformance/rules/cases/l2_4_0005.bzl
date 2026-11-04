//! case: l2_4_0005
//! doc: stdlib
//! section: 2.4
//! title: GetInt parses a well-formed integer value
//! rule: public static method GetInt(name: string): int?;    // convenience tier: null when the variable is
//! expect: run
//! stdout: 42
//! issue: 7949
//! notes: The sentence states null only for absent or malformed; the digits 42 are well-formed under any reading, so the value is the integer 42. Distinguishes a real parse from the null default -1.
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H2_INT_A", "42");
    Console.WriteLine(Environment.GetInt("BZ_H2_INT_A") ?? -1);
  end Main;
end Program;
