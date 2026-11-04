//! case: s30_5_0029
//! doc: spec
//! section: 30.5
//! title: the borrow from a Pattern A await may not be stored into an owned field
//! rule: The borrow may not flow to … An assignment to an `owned` field or
//!   owned-element collection slot — would manufacture ownership the source did
//!   not grant.
//! cites: 30.5, 28.3
//! expect: reject
//! diagnostic: owned
//! notes: s30_5_0002 is the admitted `var r := await task;` binding over the
//!   same named task.
module Conf;

public class Item
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Item;

public class Loader
  public static async method MakeAsync(): Task<Item>
    await Task.Delay(0);
    return new Item("x");
  end MakeAsync;
end Loader;

public class Holder
  private owned _held: Item? := null;

  public async method FillAsync(): Task
    using var task := Loader.MakeAsync();
    _held := await task;
  end FillAsync;
end Holder;
