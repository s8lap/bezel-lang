//! case: s8_5_1103
//! doc: spec
//! section: 8.5
//! title: a null nullable-class hole renders null and a present one dispatches
//! rule: `ToString` is invoked implicitly inside string interpolation ... via virtual dispatch
//!   ... a `null` value of a nullable-typed hole renders as the text `null` with no dispatch
//! cites: 18.5
//! expect: run
//! output: null
//!   Mid
//! issue: 8142
module Conf;

public class Mid
  public override method ToString(): string => "Mid";
end Mid;

public class Program
  public static method Main()
    var n: Mid? := null;
    var n2: Mid? := new Mid();
    Console.WriteLine($"{n}");
    Console.WriteLine($"{n2}");
  end Main;
end Program;
