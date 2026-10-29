//! case: s28_12_0056
//! doc: spec
//! section: 28.12
//! title: await using over an owning async enumerable consumed by await for disposes each item once
//! rule: await for takes ownership of each item into its iteration variable, which the body disposes explicitly once (spec 29.7, 30.7); the await using binding of the source disposes it at scope end, leaving nothing further to release (spec 28.12).
//! cites: 28.12, 29.7, 30.7
//! expect: run
//! output: ITEM
//!   ITEM
//!   3
module Conf;

public class Res: IAsyncDisposable
  public constructor() end constructor;
  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("DA");
  end DisposeAsync;
  destructor Console.WriteLine("DTOR"); end destructor;
end Res;

public class Sy
  public constructor() end constructor;
  destructor Console.WriteLine("SYNC"); end destructor;
end Sy;

public class Item
  public V: int;
  public constructor(v: int) V := v; end constructor;
  destructor Console.WriteLine("ITEM"); end destructor;
end Item;

public class Program
  public static async method Gen(): IOwningAsyncEnumerable<Item>
    yield return new Item(1);
    yield return new Item(2);
  end Gen;
  public static async method Main(): Task
    await using var src: IOwningAsyncEnumerable<Item> := Program.Gen();
    var n := 0;
    await for var it in src do n := n + it.V; it.Dispose(); end;
    Console.WriteLine(n);
  end Main;
end Program;
