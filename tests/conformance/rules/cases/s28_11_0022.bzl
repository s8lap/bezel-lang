//! case: s28_11_0022
//! doc: spec
//! section: 28.11
//! title: Counter.Increment of a plain parameter key into a field-held counter is admitted
//! rule: (ii) a method of a borrowing-family collection or of a span view that
//!   declares no return copies into its receiver the values it takes from a
//!   parameter declared `IEnumerable<T>`, a read-only span over `T`, or a bare `T`
//!   — the many-element forms ... and the single-element stores `Add`, `Insert`,
//!   `Push`, `Enqueue` and `Counter<K>.Increment`, which creates its key's entry,
//!   each of which the element slot policy already reads as a store
//! cites: 28.11
//! expect: accept
//! issue: 3848
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Tally
  private owned _tally: Counter<Item>;

  public constructor()
    _tally := new Counter<Item>();
  end constructor;

  public method Count(x: Item)
    _tally.Increment(x);
  end Count;
end Tally;
