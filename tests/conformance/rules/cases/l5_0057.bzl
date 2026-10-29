//! case: l5_0057
//! doc: stdlib
//! section: 5
//! title: Count(predicate) and Count() on an IEnumerable receiver count the matching and all elements
//! rule: IEnumerable<T> declares no Count member, so the section 5 operators Count(predicate) and Count() apply (stdlib 5, spec 26.5).
//! cites: 5, 26.5
//! expect: run
//! output: 2
//!   3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var e: IEnumerable<int> := a;
    Console.WriteLine(e.Count(lambda(x) => x > 1));
    Console.WriteLine(e.Count());
  end Main;
end Program;
