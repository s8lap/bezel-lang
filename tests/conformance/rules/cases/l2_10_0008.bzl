//! case: l2_10_0008
//! doc: stdlib
//! section: 2.10
//! title: where T: IDisposable admits an IAsyncDisposable
//! rule: stdlib §2.10 (v0.345): "The constraint `where T: IDisposable` admits every class … and
//!   the interfaces that extend `IDisposable` directly or transitively — including
//!   **`IAsyncDisposable`**, `IEnumerator<T>`, `IAsyncEnumerator<T>` …"  The argument here is
//!   bound at the `IAsyncDisposable` static type, so it is the interfaces clause that decides this
//!   call.
//! cites: 2.10, 14.2, 28.12
//! expect: run
//! stdout: used
module Conf;

public class Res: IAsyncDisposable
  public async method DisposeAsync(): Task
    Console.WriteLine("async-disposed");
  end DisposeAsync;
end Res;

public class Program
  public static method Use<T>(x: T) where T: IDisposable
    Console.WriteLine("used");
  end Use;

  public static method Main()
    var a: IAsyncDisposable := new Res();
    Use(a);
  end Main;
end Program;
