//! case: l4_9_9912
//! doc: stdlib
//! section: 4.9
//! title: Take through the null-forgiving spelling is not offered on an OwningDictionary
//! rule: The interface conformance is `IDisposable` only
//! cites: 4.9, 5, 26.5
//! expect: reject
//! diagnostic: IDisposable
//! notes: Harvested from the #9285 review (r43-rev-m145-9285). OwningDictionary conforms to IDisposable
//!   only (stdlib 4.9), so none of the IEnumerable query operators of stdlib 5 is offered on it,
//!   whatever the receiver spelling. Sibling of l4_9_9901. Spelling `!.` reads the receiver at its
//!   underlying non-nullable type (26.5 rule 2).
module Conf;

public class Box
  public V: int := 1;
end Box;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Box>();
    od["a"] := new Box();
    var od2: OwningDictionary<string, Box>? := od;
    var v := od2!.Take(1);
  end Main;
end Program;
