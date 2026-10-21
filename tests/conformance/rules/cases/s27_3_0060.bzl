//! case: s27_3_0060
//! doc: spec
//! section: 27.3
//! title: a procedure lambda with a console body selects the () -> () interface method overload
//! rule: "between overloads taking `() -> int` and `() -> ()` only the procedure slot is applicable" ([§27.3]); the lambda's body writes to the console and returns no value, so the interface call's same-name overloads select the procedure slot.
//! cites: 27.3, 22.6
//! expect: run
//! stdout: unit
//! issue: 6258
module Conf;

public interface IRun
  method Over(f: () -> int): string;
  method Over(f: () -> ()): string;
end IRun;

public class R: IRun
  public method Over(f: () -> int): string => "int";
  public method Over(f: () -> ()): string => "unit";
end R;

public class Program
  public static method Main()
    using var r := new R();
    Console.WriteLine(r.Over(lambda() => do Console.WriteLine("x"); end));
  end Main;
end Program;
