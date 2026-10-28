//! case: l2_10_0007
//! doc: stdlib
//! section: 2.10
//! title: where T: IDisposable admits an IEnumerator
//! rule: stdlib §2.10 (v0.345): "The constraint `where T: IDisposable` admits every class … and
//!   the interfaces that extend `IDisposable` directly or transitively — including
//!   `IAsyncDisposable`, **`IEnumerator<T>`**, `IAsyncEnumerator<T>` …"
//! cites: 2.10, 14.2
//! expect: run
//! stdout: used
module Conf;

public class Program
  public static method Use<T>(x: T) where T: IDisposable
    Console.WriteLine("used");
  end Use;

  public static method Main()
    var a: Array<int> := [1,2,3];
    using var e: IEnumerator<int> := a.GetEnumerator();
    Use(e);
  end Main;
end Program;
