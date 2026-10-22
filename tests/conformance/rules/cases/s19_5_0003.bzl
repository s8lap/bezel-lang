//! case: s19_5_0003
//! doc: spec
//! section: 19.5
//! title: a typed binding pattern over Object matches only non-null values
//! rule: A typed binding pattern `var n: T` also matches only non-null values — the type annotation makes it a type-constrained binding, not an unconditional binding.
//! cites: 19.5, 19.3
//! expect: run
//! output: null
//!   bind zz
module Conf;

public class Program
  public static method Describe(o: Object?): string => (case o of
    var x: Object => "bind " + x.ToString(),
    _             => "null");

  public static method Main()
    Console.WriteLine(Describe(null));
    Console.WriteLine(Describe("zz"));
  end Main;
end Program;
