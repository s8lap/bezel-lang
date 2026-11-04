//! case: s30_5_0025
//! doc: spec
//! section: 30.5
//! title: a fresh owned value is the admitted kind-1 return beside a plain class-typed parameter
//! rule: The remedies: return a fresh owned value (`return new Item();`, a named
//!   owned binding of the body, or `return move _item with fresh;`, §28.8.1 —
//!   kind 1); `return await` an admitted task source (kind 2 or 3); or expose
//!   the stored object through a synchronous property
//! cites: 30.5
//! expect: accept
//! notes: accepting twin of s30_5_0024.
module Conf;

public class Item
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Item;

public class Program
  public async method EchoAsync(i: Item): Task<Item>
    await Task.Delay(0);
    return new Item(i.Name);
  end EchoAsync;
end Program;
