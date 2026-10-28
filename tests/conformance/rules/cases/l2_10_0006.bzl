//! case: l2_10_0006
//! doc: stdlib
//! section: 2.10
//! title: where T: IDisposable admits a user interface that extends IDisposable
//! rule: stdlib §2.10 (v0.345): "The constraint `where T: IDisposable` admits every class … and
//!   the interfaces that extend `IDisposable` directly or transitively — including
//!   `IAsyncDisposable`, `IEnumerator<T>`, `IAsyncEnumerator<T>`, and **user-defined interfaces
//!   that list `IDisposable` in their extension clause**."
//! cites: 2.10, 14.2
//! expect: run
//! stdout: used
module Conf;

public interface IFoo: IDisposable
  method Bar(): int;
end IFoo;

public class Program
  public static method Use<T>(x: T) where T: IDisposable
    Console.WriteLine("used");
  end Use;

  public static method Main()
    Use(new Impl());
  end Main;

  public class Impl: IFoo
    public method Bar(): int
      return 1;
    end Bar;
  end Impl;
end Program;
