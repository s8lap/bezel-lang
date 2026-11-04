//! case: s30_5_0028
//! doc: spec
//! section: 30.5
//! title: a Pattern B await passed as a non-owned argument is rejected
//! rule: For a `OwningTask<T>` not awaited in an owned-consuming context — a
//!   bare expression-statement `await t;`, whose minted result would be
//!   dropped silently, or an await in a non-owned-consuming expression
//!   position (a non-`owned` argument, an operand of a larger expression) —
//!   the await is a compile error.
//! cites: 30.5, 28.3
//! expect: reject
//! diagnostic: owned-consuming
//! notes: s30_5_0015 pins the bare expression-statement form; the admitted
//!   spelling binds first — `using var it := await Loader.MakeAsync(); Show(it);`
//!   (s30_5_0003's shape).
module Conf;

public class Item
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Item;

public class Loader
  public static async method MakeAsync(): OwningTask<Item>
    await Task.Delay(0);
    return new Item("x");
  end MakeAsync;
end Loader;

public class Program
  public static method Show(i: Item)
    Console.WriteLine(i.Name);
  end Show;

  public static async method RunAsync(): Task
    Show(await Loader.MakeAsync());
  end RunAsync;
end Program;
