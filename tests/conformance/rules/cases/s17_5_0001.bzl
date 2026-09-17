//! case: s17_5_0001
//! doc: spec
//! section: 17.5
//! title: in evaluates to Contains on an Array
//! rule: "`x in collection` evaluates to `true` if `collection.Contains(x)` is
//!   true." `in` produces `bool`.
//! cites: 17.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var numbers: Array<int> := [10, 20, 42];
    var found := 42 in numbers;
    Console.WriteLine(found);
  end Main;
end Program;
