//! case: l4_9_9903
//! doc: stdlib
//! section: 4.9
//! title: SingleOrNull is not offered on an OwningDictionary
//! rule: The interface conformance is `IDisposable` only
//! cites: 4.9, 5, 4.8
//! expect: reject
//! diagnostic: IDisposable
//! issue: 9262
//! notes: Harvested from the #9262 review (od.bzl). OwningDictionary conforms to IDisposable only (stdlib
//!   4.9), so the IEnumerable query operators of stdlib 5 are not offered on it. Sibling of l4_9_2101
//!   (ElementAtOrNull).
module Conf;

public class Box
  public V: int := 1;
end Box;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Box>();
    od["a"] := new Box();
    var v := od.SingleOrNull();
    if v = null then
      Console.WriteLine("none");
    end if;
  end Main;
end Program;
