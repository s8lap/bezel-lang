//! case: s28_12_0032
//! doc: spec
//! section: 28.12
//! title: a value held at an IAsyncDisposable static type has the synchronous Dispose() too
//! rule: Language spec §28.12 (v0.345): "`IAsyncDisposable` extends `IDisposable`, so it
//!   conforms to `IDisposable` under §14.2's rule 3: a value held at that interface type has the
//!   synchronous `Dispose()` — the implementing class's compiler-generated one — beside
//!   `DisposeAsync()`".
//! cites: 28.12, 14.2
//! expect: run
//! stdout: dtor
module Conf;

public class AsyncRes: IAsyncDisposable
  destructor
    Console.WriteLine("dtor");
  end destructor;

  public async method DisposeAsync(): Task
    Console.WriteLine("async");
  end DisposeAsync;
end AsyncRes;

public class Program
  public static method Main()
    var r := new AsyncRes();
    var d: IAsyncDisposable := move r;
    d.Dispose();
    Console.WriteLine("done");
  end Main;
end Program;
