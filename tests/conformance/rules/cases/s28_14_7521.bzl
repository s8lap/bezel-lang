//! case: s28_14_7521
//! doc: spec
//! section: 28.14
//! title: a local of the constructor passed as a scoped source into an owned field of this is rejected
//! rule: A bounding object that is none of the admitted resolutions
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
  public constructor()
    using var x := new Item("tmp");
    _h := new Holder(scoped x);
  end constructor;
  public method Get(): string => _h!.Get();
end Owner;
public class Program
  public static method Main(): int
    using var o := new Owner();
    Console.WriteLine(o.Get());
    return 0;
  end Main;
end Program;
