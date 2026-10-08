//! case: s28_3_0111
//! doc: spec
//! section: 28.3
//! title: an indexer getter may not return a capture-bearing function value
//! rule: A function-typed holder is a field (a `scoped` field excepted), ... or
//!   the enclosing method's return or `yield return` slot (a property or
//!   indexer getter's included), whose declared type ... is a function type
//!   ... At every function-typed holder the closure bullet's sink rule holds:
//!   a store of a function value admits a capture-free value and rejects one
//!   whose source set is non-empty
//! cites: 28.3
//! expect: reject
//! diagnostic: source set
//! issue: 5621
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Table
  public constructor()
  end constructor;

  public property [key: Item]: () -> int => lambda() => key.Size();
end Table;
