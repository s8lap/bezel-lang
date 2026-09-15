//! case: s17_7_0005
//! doc: spec
//! section: 17.7
//! title: the new() shorthand is admitted at a typed local initializer
//! rule: "A field initializer or typed local declaration may use the `new()`
//!   shorthand when the type is unambiguous: `var xs: Array<int> := new();`"
//! cites: 17.7
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := new();
    Console.WriteLine(xs.Count);
  end Main;
end Program;
