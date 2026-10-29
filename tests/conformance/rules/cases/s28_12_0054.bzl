//! case: s28_12_0054
//! doc: spec
//! section: 28.12
//! title: await using over an IDisposable-typed class without DisposeAsync runs its destructor once
//! rule: A class with no DisposeAsync reaches the synchronous-disposal fallback of an await using binding: its destructor body runs once at scope end (spec 28.12).
//! cites: 28.12, 28.10
//! expect: run
//! output: body
//!   SYNC
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
  public static async method Main(): Task
    await using var d: IDisposable := new Sy();
    Console.WriteLine("body");
  end Main;
end Program;
