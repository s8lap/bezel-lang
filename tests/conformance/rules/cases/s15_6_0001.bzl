//! case: s15_6_0001
//! doc: spec
//! section: 15.6
//! title: range slicing an Array<T> produces a fresh owned array
//! rule: Range slicing `arr[a..b]` is supported on `Array<T>`; the produced
//!   array is owned by the caller (§15.6).
//! cites: 15.6
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    using var a: Array<int> := [10, 20, 30, 40];
    using var s := a[1..2];
    Console.WriteLine(s.Count);
  end Main;
end Program;
