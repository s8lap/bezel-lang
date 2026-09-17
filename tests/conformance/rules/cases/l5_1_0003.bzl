//! case: l5_1_0003
//! doc: stdlib
//! section: 5.1
//! title: MinOrNull() on an empty source returns null
//! rule: method MinOrNull(): T?; // null if empty.
//! cites: 5.1
//! expect: run
//! stdout: null
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    var m := xs.MinOrNull();
    Console.WriteLine($"{m}");
  end Main;
end Program;
