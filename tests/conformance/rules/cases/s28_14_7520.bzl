//! case: s28_14_7520
//! doc: spec
//! section: 28.14
//! title: a constructor parameter passed as a scoped source into an owned field of this is rejected
//! rule: and is admitted under the bounding-object forms above with *the argument's borrow source* in
//!   place of a closure's capture set
//! cites: 28.14
//! expect: reject
//! diagnostic: bounding object
module Conf;
import System;

public class Item
  private _n: string;
  public constructor(n: string)
    _n := n;
  end constructor;
  public property N: string => _n;
end Item;
public class Holder
  private _it: Item;
  public constructor(scoped it: Item)
    _it := it;
  end constructor;
  public method Get(): string => _it.N;
end Holder;

public class Owner
  private owned _h: Holder?;
  public constructor(x: Item)
    _h := new Holder(scoped x);
  end constructor;
  public method Get(): string => _h!.Get();
end Owner;
public class Program
  public static method Main(): int
    using var it := new Item("tmp");
    using var o := new Owner(it);
    Console.WriteLine(o.Get());
    return 0;
  end Main;
end Program;
