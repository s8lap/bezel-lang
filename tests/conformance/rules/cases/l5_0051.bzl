//! case: l5_0051
//! doc: stdlib
//! section: 5
//! title: Count(predicate) on a Dictionary is rejected because the Count property blocks the extension
//! rule: An instance member blocks a same-named extension by name, any arity (spec 26.5 rule 1); Dictionary<K, V> declares the property Count (stdlib 4), so no section 5 Count operator is reachable through it.
//! cites: 26.5, 4
//! expect: reject
//! diagnostic: Count
//! issue: 7597
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<int, int>();
    Console.WriteLine(d.Count(lambda(x) => true));
  end Main;
end Program;
