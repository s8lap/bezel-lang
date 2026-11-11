//! case: s7_5_4505
//! doc: spec
//! section: 7.5
//! title: property and method suffixes after a null-conditional member read the live
//!   object when present
//! rule: **The chain is the whole postfix sequence**
//! cites: 7.4
//! expect: run
//! output: after add: 1
//!   k: 1
//!   c: True
//!   after 3: 3
//! issue: 6070
module Conf;

public class Box
  public owned Items: Array<int> := [];
end Box;

public class Program
  public static method Main()
    var a: Box? := new Box();
    a?.Items.Add(5);
    Console.WriteLine($"after add: {a!.Items.Count}");
    var k: int? := a?.Items.Count;
    Console.WriteLine($"k: {k}");
    var c: bool? := a?.Items.Contains(5);
    Console.WriteLine($"c: {c}");
    a?.Items.Add(6);
    a?.Items.Add(7);
    Console.WriteLine($"after 3: {a!.Items.Count}");
  end Main;
end Program;
