//! case: s5_1_0176
//! doc: spec
//! section: 5.1
//! title: a lazy Where result converts to IEnumerable and Object, and ToArray materializes an Array
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this document
//! cites: 5.1, stdlib 5
//! expect: run
//! issue: 8098
//! output: 3
//!   4
//!   1
//!   7

module Conf;

public class Program
  public static method Tag(o: Object): int => 7;

  public static method Main()
    var a: Array<int> := [1, 2, 3, 4];
    var e: IEnumerable<int> := a.Where(lambda(x) => x > 2);
    for var x in e do
      Console.WriteLine(x);
    end for;
    using var q: Array<int> := a.Where(lambda(x) => x > 3).ToArray();
    Console.WriteLine(q.Count);
    Console.WriteLine(Tag(a.Where(lambda(x) => x > 2)));
  end Main;
end Program;
