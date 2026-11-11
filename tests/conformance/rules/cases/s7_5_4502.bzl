//! case: s7_5_4502
//! doc: spec
//! section: 7.5
//! title: the argument expressions of a skipped chain suffix are not evaluated, those
//!   of a taken one are
//! rule: the argument and index expressions of the skipped suffixes are not evaluated
//!   either
//! cites: 22.5
//! expect: run
//! output: side 1
//!   side 0
//!   side 7
//!   count: 2 first: 7
//! issue: 6070
module Conf;

public class Box
  public owned Items: Array<int> := [];
end Box;

public class Program
  private static method Side(v: int): int
    Console.WriteLine($"side {v}");
    return v;
  end Side;
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    a?.Items.Add(Side(1));
    n?.Items.Add(Side(2));
    a?.Items.Insert(Side(0), Side(7));
    n?.Items.Insert(Side(0), Side(8));
    Console.WriteLine($"count: {a!.Items.Count} first: {a!.Items[0]}");
  end Main;
end Program;
