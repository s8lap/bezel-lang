//! case: s17_10_0005
//! doc: spec
//! section: 17.10
//! title: distinct generic instantiations produce distinct Type values
//! rule: "For generic instantiations, each substitution distinct up to
//!   top-level nullability produces a distinct `Type`: `typeof(Array<int>)`
//!   and `typeof(Array<string>)` are different `Type` values."
//! cites: 17.10
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    var a: Type := typeof(Array<int>);
    var b: Type := typeof(Array<string>);
    Console.WriteLine(a = b);
  end Main;
end Program;
