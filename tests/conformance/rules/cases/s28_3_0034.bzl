//! case: s28_3_0034
//! doc: spec
//! section: 28.3
//! title: consuming an owned binding while a live local borrow of it exists is rejected
//! rule: The consume-site freeze: consuming an owned binding while a live local borrow
//!   of its current value exists is a compile error.
//! expect: reject
//! diagnostic: live
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
    var b := item;
    pool.Add(move item);
    b.Configure();
  end Demo;
end Program;
