//! case: s30_5_0030
//! doc: spec
//! section: 30.5
//! title: assigning a producer await to an existing local is not a scoped await-bind and is rejected
//! rule: The near-miss forms stay rejected … `x := await FetchAsync(url);` at
//!   an existing local, whose scope may strictly enclose this statement's
//!   block (the message names the fresh-declaration condition and offers the
//!   two-line form)
//! cites: 30.5, 28.3
//! expect: reject
//! diagnostic: fresh
//! notes: x is first bound through the admitted two-line form (a using var
//!   task, then `var x := await first;`), so the reassignment from an unbound
//!   producer is the only defect, whatever the scoped await-bind's status.
module Conf;

public class Item
  public Name: string;
  public constructor(name: string) Name := name; end constructor;
end Item;

public class Loader
  public static async method MakeAsync(name: string): Task<Item>
    await Task.Delay(0);
    return new Item(name);
  end MakeAsync;
end Loader;

public class Program
  public static async method RunAsync(): Task
    using var first := Loader.MakeAsync("a");
    var x := await first;
    x := await Loader.MakeAsync("b");
    Console.WriteLine(x.Name);
  end RunAsync;
end Program;
