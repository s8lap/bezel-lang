//! case: s8_5_1102
//! doc: spec
//! section: 8.5
//! title: a hole dispatches to an inherited ToString override and to an override that calls base.ToString()
//! rule: `ToString` is invoked implicitly inside string interpolation ... via virtual dispatch
//! cites: 18.5
//! expect: run
//! output: Mid
//!   Leaf2<Mid>
//! issue: 8142
module Conf;

public class Mid
  public override method ToString(): string => "Mid";
end Mid;

public class Leaf: Mid
end Leaf;

public class Leaf2: Mid
  public override method ToString(): string => $"Leaf2<{base.ToString()}>";
end Leaf2;

public class Program
  public static method Main()
    var m: Mid := new Leaf();
    var m2: Mid := new Leaf2();
    Console.WriteLine($"{m}");
    Console.WriteLine($"{m2}");
  end Main;
end Program;
