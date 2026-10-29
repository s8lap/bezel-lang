//! case: l5_0036
//! doc: stdlib
//! section: 5
//! title: assigning the int result of First on an IList of int to a string local is rejected
//! rule: First() returns T (stdlib 5), here int; an int does not convert implicitly to string (spec 5.1), so the initializer is a type error.
//! cites: 5, 5.1
//! expect: reject
//! diagnostic: convert
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    var s: string := l.First();
    Console.WriteLine(s);
  end Main;
end Program;
