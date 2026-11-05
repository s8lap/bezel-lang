//! case: l9_7_2301
//! doc: stdlib
//! section: 9.7
//! title: a predicate Count on an Array is refused, the operator is
//!   composed through Where
//! rule: `arr.Count(pred)` does not resolve to LINQ's predicate count;
//!   compose `arr.Where(pred).Count()`.
//! cites: 4.4, 26.5
//! expect: reject
//! diagnostic: instance member
//! notes: Green on base and tip (control for the sorted-collection rows).
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(a.Count(lambda(x: int) => x > 1));
  end Main;
end Program;
