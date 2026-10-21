//! case: s27_3_0063
//! doc: spec
//! section: 27.3
//! title: a throw-only procedure lambda selects the () -> () interface method overload, not the () -> int one
//! rule: "A body none of whose paths returns at all, every path throwing or looping forever, executes no value-carrying `return` and is a procedure lambda too" ([§27.3]), and "between overloads taking `() -> int` and `() -> ()` only the procedure slot is applicable" ([§27.3]).
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
    Console.WriteLine(r.Over(lambda() => do throw new InvalidOperationException("e"); end));
  end Main;
end Program;
