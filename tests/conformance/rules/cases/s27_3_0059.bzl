//! case: s27_3_0059
//! doc: spec
//! section: 27.3
//! title: a procedure lambda with a console body selects the () -> () extension overload
//! rule: "between overloads taking `() -> int` and `() -> ()` only the procedure slot is applicable" ([§27.3]); the lambda's body writes to the console and returns no value, so an extension method's same-name overloads select the procedure slot.
//! cites: 27.3, 22.6
//! expect: run
//! stdout: unit
//! issue: 6258
module Conf;

extension string
  public method Over(f: () -> int): string => "int";
  public method Over(f: () -> ()): string => "unit";
end;

public class Program
  public static method Main()
    Console.WriteLine("a".Over(lambda() => do Console.WriteLine("x"); end));
  end Main;
end Program;
