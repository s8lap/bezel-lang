//! case: s5_1_0174
//! doc: spec
//! section: 5.1
//! title: a lazy Where result returned from an owned Array method is not implicitly convertible
//! rule: The following conversions are implicit, and the term *implicit conversion* throughout this document
//! cites: 5.1, stdlib 5
//! expect: reject
//! diagnostic: lists no implicit conversion from a sequence
//! issue: 8098
module Conf;

public class Program
  public static method RetArr(a: Array<int>): owned Array<int> => a.Where(lambda(x) => x > 1);

  public static method Main()
    var a: Array<int> := [1, 2, 3, 4];
    using var q := RetArr(a);
    Console.WriteLine(q.Count);
  end Main;
end Program;
