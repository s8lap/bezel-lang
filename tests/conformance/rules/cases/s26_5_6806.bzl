//! case: s26_5_6806
//! doc: spec
//! section: 26.5
//! title: the null-conditional call spelling of the Dictionary property Keys is refused
//! rule: the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.5, 7.5, 27.7, 4.8
//! expect: reject
//! diagnostic: instance member
//! issue: 8650
//! notes: `Keys` is `readonly property Keys: IReadOnlyCollection<K>` on Dictionary<K, V> (stdlib 4.8): declared, so the
//!   IEnumerable `Keys()`-style extension lookup is never consulted and the property has no call candidate.
module Conf;

public class Program
  public static method Main()
    using var d: Dictionary<string, int> := new Dictionary<string, int>();
    var m: Dictionary<string, int>? := d;
    var k := m?.Keys();
  end Main;
end Program;
