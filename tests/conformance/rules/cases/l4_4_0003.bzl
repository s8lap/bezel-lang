//! case: l4_4_0003
//! doc: stdlib
//! section: 4.4
//! title: IndexOf finds the first occurrence at or after startIndex
//! rule: method IndexOf(item: T, startIndex: int := 0): int; // first
//!   occurrence at or after startIndex, or -1 (v0.228).
//! cites: 4.4
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int> := [7, 7, 7, 9];
    Console.WriteLine(arr.IndexOf(7, 2));
  end Main;
end Program;
