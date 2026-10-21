//! case: s27_3_0061
//! doc: spec
//! section: 27.3
//! title: a procedure lambda with a console body selects the () -> () class method overload
//! rule: "between overloads taking `() -> int` and `() -> ()` only the procedure slot is applicable" ([§27.3]); the lambda's body writes to the console and returns no value, so the class method's same-name overloads select the procedure slot.
//! cites: 27.3, 22.6
//! expect: run
//! stdout: unit
module Conf;

public class R
  public method Over(f: () -> int): string => "int";
  public method Over(f: () -> ()): string => "unit";
end R;

public class Program
  public static method Main()
    using var r := new R();
    Console.WriteLine(r.Over(lambda() => do Console.WriteLine("x"); end));
  end Main;
end Program;
