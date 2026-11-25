//! case: s23_1_9603
//! doc: spec
//! section: 23.1
//! title: assigning Length of a FrozenArray<T> field inside a generic class is rejected
//! rule: A `readonly` auto-property may be assigned only in a field initializer or
//!   in a constructor
//! cites: 23.1, 17.6
//! expect: reject
//! diagnostic: read-only
//! issue: 9214
module Conf;

public class Box<T>
  public F: FrozenArray<T>;

  public constructor(f: FrozenArray<T>)
    this.F := f;
  end constructor;

  public method Poke()
    this.F.Length := 3;
  end Poke;
end Box;
