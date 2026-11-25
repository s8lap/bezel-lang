//! case: l4_9_9910
//! doc: stdlib
//! section: 4.9
//! title: A chained Where then Select is not offered on an OwningDictionary
//! rule: The interface conformance is `IDisposable` only
//! cites: 4.9, 5, 26.5
//! expect: reject
//! diagnostic: IDisposable
//! notes: Harvested from the #9285 review (r43-rev-m145-9285). OwningDictionary conforms to IDisposable
//!   only (stdlib 4.9), so none of the IEnumerable query operators of stdlib 5 is offered on it,
//!   whatever the receiver spelling. Sibling of l4_9_9901. Chained form: the first operator already
//!   has no member.
module Conf;

public class Box
  public V: int := 1;
end Box;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Box>();
    od["a"] := new Box();

    var v := od.Where(lambda(x) => true).Select(lambda(x) => 1);
  end Main;
end Program;
