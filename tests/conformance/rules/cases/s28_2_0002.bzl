//! case: s28_2_0002
//! doc: spec
//! section: 28.2
//! title: Dispose through a record receiver has no such member
//! rule: `Dispose()` is not on `Object`; it is class-only. Records, enums, primitives,
//!   and inline arrays do not implement `IDisposable`.
//! expect: reject
//! diagnostic: Dispose
module Conf;

record Money(Amount: decimal);

public class Program
  public method Demo(m: Money)
    m.Dispose();
  end Demo;
end Program;
