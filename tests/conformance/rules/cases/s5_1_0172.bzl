//! case: s5_1_0172
//! doc: spec
//! section: 5.1
//! title: a lazy Where result bound to an Array local is not implicitly convertible
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this document
//! cites: 5.1, stdlib 5
//! expect: reject
//! diagnostic: lists no implicit conversion from a sequence
//! issue: 8098
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3, 4];
    var q: Array<int> := a.Where(lambda(x) => x > 2);
    Console.WriteLine(q.Count);
  end Main;
end Program;
