//! case: s7_5_4501
//! doc: spec
//! section: 7.5
//! title: a null-conditional chain ending in a void collection method calls on a
//!   present receiver only
//! rule: **The chain is the whole postfix sequence**
//! cites: 7.4
//! expect: run
//! output: add: 4
//!   sort: 1 5
//!   reverse: 5
//!   insert: 9 5
//!   removeat: 5 4
//!   clear: 0
//!   null ok
//! issue: 6070
module Conf;

public class Box
  public owned Items: Array<int> := [3, 1, 2];
end Box;

public class Program
  public static method Main()
    var a: Box? := new Box();
    var n: Box? := null;
    a?.Items.Add(5);
    Console.WriteLine($"add: {a!.Items.Count}");
    a?.Items.Sort();
    Console.WriteLine($"sort: {a!.Items[0]} {a!.Items[3]}");
    a?.Items.Reverse();
    Console.WriteLine($"reverse: {a!.Items[0]}");
    a?.Items.Insert(0, 9);
    Console.WriteLine($"insert: {a!.Items[0]} {a!.Items.Count}");
    a?.Items.RemoveAt(0);
    Console.WriteLine($"removeat: {a!.Items[0]} {a!.Items.Count}");
    a?.Items.Clear();
    Console.WriteLine($"clear: {a!.Items.Count}");
    n?.Items.Add(5);
    n?.Items.Sort();
    n?.Items.Clear();
    Console.WriteLine("null ok");
  end Main;
end Program;
