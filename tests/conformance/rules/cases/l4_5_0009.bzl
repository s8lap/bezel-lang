//! case: l4_5_0009
//! doc: stdlib
//! section: 4.5
//! title: an undischarged owning-drain iteration variable auto-drops with the must-use warning
//! rule: The spec §28.3 discharge rules apply per iteration in the drain
//!   loop; an undischarged element auto-drops at the body's end with the
//!   must-use warning.
//! cites: 4.5, 28.11, 29.2
//! expect: warn
//! diagnostic: must use
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Drop()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    for var w in pool.Drain() do
    end;
  end Drop;
end Program;
