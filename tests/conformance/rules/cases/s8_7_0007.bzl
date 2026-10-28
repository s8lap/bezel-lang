//! case: s8_7_0007
//! doc: spec
//! section: 8.7
//! title: DisposeAsync on a user interface that does not extend IAsyncDisposable is a compile error
//! rule: Language spec §8.7 (v0.345): "`DisposeAsync()` is a member only of a type that declares
//!   or inherits it — `IAsyncDisposable`, the interfaces extending it, a class implementing it".
//! cites: 8.7, 28.12
//! expect: reject
//! diagnostic: DisposeAsync
module Conf;

public interface IFoo
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
    x.DisposeAsync();
  end Main;
end Program;
