//! case: l4_8_0006
//! doc: stdlib
//! section: 4.8
//! title: there is no Set method on Dictionary<K,V> — writes go through the indexer
//! rule: There is no separate Set method — neither here nor on any other
//!   collection. A call to a Set method on any collection is rejected as
//!   an unknown member.
//! cites: 4.8
//! expect: reject
//! diagnostic: Set
module Conf;

public class Program
  public method Demo()
    using var d := new Dictionary<string, int>();
    d.Set("a", 1);
  end Demo;
end Program;
