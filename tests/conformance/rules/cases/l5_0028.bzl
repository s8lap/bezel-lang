//! case: l5_0028
//! doc: stdlib
//! section: 5
//! title: Count(predicate) on an IReadOnlyList is rejected: the Count property blocks the extension
//! rule: An instance member blocks a same-named extension by name, any arity (spec 26.5 rule 1); the spelling that reaches the operator pipeline is r.Where(pred).Count().
//! cites: 26.5, 4.1
//! expect: reject
//! diagnostic: Count
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(r.Count(lambda(x) => x > 3));
  end Main;
end Program;
