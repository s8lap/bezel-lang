//! case: s7_1_0008
//! doc: spec
//! section: 7.1
//! title: a nullable decimal argument at Math.Clamp is rejected
//! rule: `string` and `string?` are distinct types. The compiler enforces the distinction at every assignment, parameter pass, and return.
//! cites: 2.2, 14.2
//! expect: reject
//! diagnostic: nullable
//! issue: 7660
//! notes: decimal twin of the int row: the rule does not depend on the element type.
module Conf;

public class Program
  public static method Pick(d: decimal?): decimal
    return Math.Clamp(d, 1m, 9m);
  end Pick;

  public static method Main()
    Console.WriteLine(0);
  end Main;
end Program;
