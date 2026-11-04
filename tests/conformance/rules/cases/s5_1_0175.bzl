//! case: s5_1_0175
//! doc: spec
//! section: 5.1
//! title: a lazy OrderBy result bound to an Array local is not implicitly convertible
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this document
//! cites: 5.1, stdlib 5
//! expect: reject
//! diagnostic: lists no implicit conversion from a sequence
//! issue: 8098
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [3, 1, 2];
    var q: Array<int> := a.OrderBy(lambda(x) => x);
    Console.WriteLine(q.Count);
  end Main;
end Program;
