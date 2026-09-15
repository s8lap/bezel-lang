//! case: l4_2_0006
//! doc: stdlib
//! section: 4.2
//! title: OwningDictionary<K, V> rejects a value-typed V at instantiation
//! rule: OwningDictionary<K, V> requires where V: class (the value type is
//!   owned); a value-typed V has no disposal obligation and is rejected at
//!   the instantiation site, keys being unconstrained.
//! cites: 4.2, 4.9, 14.4
//! expect: reject
//! diagnostic: class
module Conf;

public class Program
  public method Demo()
    using var m := new OwningDictionary<string, int>();
  end Demo;
end Program;
