//! case: s28_12_0055
//! doc: spec
//! section: 28.12
//! title: await using over an owning async enumerator disposes the enumerator and the taken item once
//! rule: An await using binding over an IOwningAsyncEnumerator is disposed at scope end; an item taken by using var is disposed at its own scope end, before the following statement, and the unproduced second item never exists (spec 28.12, 29.1).
//! cites: 28.12, 29.1, 30.7
//! expect: run
//! output: 7
//!   ITEM
//!   body
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
  public static async method Gen(): IOwningAsyncEnumerator<Item>
    yield return new Item(7);
    yield return new Item(8);
  end Gen;
  public static async method Main(): Task
    await using var en: IOwningAsyncEnumerator<Item> := Program.Gen();
    var b := await en.MoveNextAsync();
    if b then using var it := en.TakeCurrent(); Console.WriteLine(it.V); end;
    Console.WriteLine("body");
  end Main;
end Program;
