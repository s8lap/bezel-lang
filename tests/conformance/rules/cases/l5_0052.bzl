//! case: l5_0052
//! doc: stdlib
//! section: 5
//! title: Count(predicate) on a Set receiver is rejected because the Count property blocks the extension
//! rule: An instance member blocks a same-named extension by name, any arity (spec 26.5 rule 1); Set<T> declares the property Count (stdlib 4), so no section 5 Count operator is reachable through it.
//! cites: 26.5, 4
//! expect: reject
//! diagnostic: Count
//! issue: 7597
module Conf;

public class Program
  public static method Main()
    using var d := new Set<int>();
    Console.WriteLine(d.Count(lambda(x) => x > 1));
  end Main;
end Program;
