//! case: s28_3_0035
//! doc: spec
//! section: 28.3
//! title: transfer first, then re-borrow from the new owner, is the freeze's mechanical rewrite
//! rule: The mechanical rewrite for the frozen case is transfer first, then re-borrow
//!   from the new owner.
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Configure()
  end Configure;
end Item;

public class Program
  public method Demo(pool: OwningArray<Item>)
    var item := new Item();
    pool.Add(move item);
    var b := pool[pool.Count - 1];
    b.Configure();
  end Demo;
end Program;
