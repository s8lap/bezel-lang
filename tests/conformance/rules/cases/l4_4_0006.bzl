//! case: l4_4_0006
//! doc: stdlib
//! section: 4.4
//! title: Sorted() natural order requires T: IComparable<T>, checked at the call
//! rule: method Sorted(): owned Array<T>; // natural order; admitted only
//!   where T: IComparable<T>, checked at the call's instantiation (v0.275).
//! cites: 4.4, 14.2
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    using var arr: Array<int> := [3, 1, 2];
    using var sorted := arr.Sorted();
    Console.WriteLine(sorted[0]);
  end Main;
end Program;
