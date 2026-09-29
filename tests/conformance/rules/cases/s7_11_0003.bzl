//! case: s7_11_0003
//! doc: spec
//! section: 7.11
//! title: the relational operator < does not lift over a nullable operand
//! rule: No operator lifts to a nullable type; `<`, `<=`, `>`, `>=` are
//!   rejected over a nullable operand because an ordering comparison over an
//!   absent participant has no defensible result.
//! cites: C.1
//! expect: reject
//! diagnostic: lifts
module Conf;

public class Program
  public static method Main()
    var a: int? := int.ParseOrNull("5");
    var b: int := 10;
    if a < b then
      Console.WriteLine("less");
    end;
  end Main;
end Program;
