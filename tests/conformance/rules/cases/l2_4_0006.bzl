//! case: l2_4_0006
//! doc: stdlib
//! section: 2.4
//! title: GetInt parses a well-formed negative integer value
//! rule: public static method GetInt(name: string): int?;    // convenience tier: null when the variable is
//! expect: run
//! stdout: -7
//! issue: 7949
//! notes: Negative digits are well-formed under any reading.
module Conf;

public class Program
  public static method Main()
    Environment.Set("BZ_H2_INT_B", "-7");
    Console.WriteLine(Environment.GetInt("BZ_H2_INT_B") ?? 99);
  end Main;
end Program;
