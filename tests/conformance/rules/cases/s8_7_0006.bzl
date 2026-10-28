//! case: s8_7_0006
//! doc: spec
//! section: 8.7
//! title: Dispose on a user interface that does not extend IDisposable is a compile error
//! rule: Language spec §8.7 (v0.345): "On `Object`, on a runtime-managed value and on a value of
//!   any other interface type … a user interface that neither extends `IDisposable` nor declares
//!   the member — the name resolves to no member, so a call in any spelling (`x.Dispose()`,
//!   `x?.Dispose()`, `x!.Dispose()`) … are compile errors at the type level".
//! cites: 8.7
//! expect: reject
//! diagnostic: Dispose
//! issue: 7273
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
    x.Dispose();
  end Main;
end Program;
