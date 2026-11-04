//! case: l2_4_0012
//! doc: stdlib
//! section: 2.4
//! title: GetBool returns null for an absent variable
//! rule: public static method GetBool(name: string): bool?;  //   absent OR malformed — deliberately conflated
//! expect: run
//! stdout: True
//! issue: 7949
//! notes: The declaration comment: null when the variable is absent OR malformed, conflated.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Environment.GetBool("BZ_H2_BOOL_ABSENT_d77a") = null);
  end Main;
end Program;
