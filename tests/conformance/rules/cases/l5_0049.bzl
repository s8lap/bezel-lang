//! case: l5_0049
//! doc: stdlib
//! section: 5
//! title: Count(predicate) on an IList receiver is rejected because the Count property blocks the extension
//! rule: An instance member blocks a same-named extension by name, any arity (spec 26.5 rule 1); IList<T> declares the property Count (stdlib 4.1), so no section 5 Count operator is reachable through it.
//! cites: 26.5, 4.1
//! expect: reject
//! diagnostic: Count
//! issue: 7597
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var l: IList<int> := a;
    Console.WriteLine(l.Count(lambda(x) => x > 1));
  end Main;
end Program;
