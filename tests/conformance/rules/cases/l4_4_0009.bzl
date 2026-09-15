//! case: l4_4_0009
//! doc: stdlib
//! section: 4.4
//! title: Sort mutates the receiver in place and returns nothing
//! rule: The imperative-verb form (Sort) mutates the receiver in place and
//!   returns nothing.
//! cites: 4.4
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int> := [3, 1, 2];
    arr.Sort(lambda(a: int, b: int): int => a - b);
    Console.WriteLine(arr[0]);
  end Main;
end Program;
