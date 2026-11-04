//! case: s5_1_0173
//! doc: spec
//! section: 5.1
//! title: a lazy Where result passed to an Array parameter is not implicitly convertible
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this document
//! cites: 5.1, stdlib 5
//! expect: reject
//! diagnostic: lists no implicit conversion from a sequence
//! issue: 8098
module Conf;

public class Program
  public static method TakeArr(x: Array<int>): int => x.Count;

  public static method Main()
    var a: Array<int> := [1, 2, 3, 4];
    Console.WriteLine(TakeArr(a.Where(lambda(x) => x > 2)));
  end Main;
end Program;
