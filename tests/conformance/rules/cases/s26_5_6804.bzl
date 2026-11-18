//! case: s26_5_6804
//! doc: spec
//! section: 26.5
//! title: the null-conditional call spelling of the LinkedList property First is refused
//! rule: the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.5, 7.5, 27.7, 4.6
//! expect: reject
//! diagnostic: instance member
//! issue: 8650
//! notes: `First` is `pure readonly property First: T?` on LinkedList<T> (stdlib 4.6): declared, so the IEnumerable
//!   `First()` extension is never consulted (26.5 rule 1) and the property has no call candidate.
module Conf;

public class Program
  public static method Main()
    using var l := new LinkedList<int>();
    var m: LinkedList<int>? := l;
    var x := m?.First();
  end Main;
end Program;
