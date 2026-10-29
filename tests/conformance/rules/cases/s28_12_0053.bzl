//! case: s28_12_0053
//! doc: spec
//! section: 28.12
//! title: await using over an IDisposable-typed binding awaits the run-time DisposeAsync, not the destructor
//! rule: An await using binding's disposal is chosen by the run-time class: a class with DisposeAsync has it awaited at scope end, and its destructor body is superseded, so DA prints and DTOR does not (spec 28.12).
//! cites: 28.12, 28.10
//! expect: run
//! output: body
//!   DA
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
    await using var d: IDisposable := new Res();
    Console.WriteLine("body");
  end Main;
end Program;
