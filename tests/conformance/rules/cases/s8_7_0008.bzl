//! case: s8_7_0008
//! doc: spec
//! section: 8.7
//! title: Dispose is a member of an interface that extends IDisposable
//! rule: Language spec §8.7 (v0.345): "`Dispose()` is a member of a class type, of an interface
//!   type that is or extends `IDisposable` — the interfaces `where T: IDisposable` admits … and of
//!   nothing else".
//! cites: 8.7
//! expect: run
//! stdout: ok
module Conf;

public interface IFoo: IDisposable
  method Bar(): int;
end IFoo;

public class Impl: IFoo
  public method Bar(): int
    return 1;
  end Bar;
end Impl;

public class Program
  public static method Main()
    var x: IFoo := new Impl();
    x.Dispose();
    Console.WriteLine("ok");
  end Main;
end Program;
