//! case: l2_4_0007
//! doc: stdlib
//! section: 2.4
//! title: GetInt returns null for an absent variable
//! rule: public static method GetInt(name: string): int?;    // convenience tier: null when the variable is
//! expect: run
//! stdout: -1
//! issue: 7949
//! notes: Absent half of the conflation; l2_4_0004 pins the malformed half.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Environment.GetInt("BZ_H2_INT_ABSENT_c04e") ?? -1);
  end Main;
end Program;
