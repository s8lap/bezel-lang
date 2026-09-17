//! case: l4_4_0008
//! doc: stdlib
//! section: 4.4
//! title: Sorted leaves the receiver unchanged and returns a fresh copy
//! rule: The past-participle form (Sorted) leaves the receiver unchanged
//!   and returns a fresh owned Array<T> with the elements in the new
//!   order.
//! cites: 4.4
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int> := [3, 1, 2];
    using var sorted := arr.Sorted(lambda(a: int, b: int): int => a - b);
    Console.WriteLine(arr[0]);
  end Main;
end Program;
