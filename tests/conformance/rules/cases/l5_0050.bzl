//! case: l5_0050
//! doc: stdlib
//! section: 5
//! title: Count(predicate) as an argument of a nested call on an IList receiver is rejected
//! rule: An instance member blocks a same-named extension by name, any arity (spec 26.5 rule 1); the rejection holds when the call is an argument of Math.Max, not only at statement level.
//! cites: 26.5, 4.1
//! expect: reject
//! diagnostic: Count
//! issue: 7597
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var l: IList<int> := a;
    Console.WriteLine(Math.Max(1, l.Count(lambda(x) => x > 1)));
  end Main;
end Program;
