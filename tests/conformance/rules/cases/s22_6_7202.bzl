//! case: s22_6_7202
//! doc: spec
//! section: 22.6
//! title: a unit-bodied lambda selects the procedure-slot member of a
//!   generic extension overload pair
//! rule: a unit-typed body or a `: ()` ascription is exact
//! cites: 22.6, 26.5, 27.3
//! expect: run
//! stdout: unit
module Conf;

public extension Array<T>
  public method E(f: () -> T): T
    return f();
  end E;

  public method E(f: () -> ()): string
    return "unit";
  end E;
end extension;

public class Program
  public static method Main()
    var xs: Array<int> := [1];
    Console.WriteLine(xs.E(lambda() => do Console.Write("p"); end));
  end Main;
end Program;
