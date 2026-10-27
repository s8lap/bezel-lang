//! case: s28_2_0002
//! doc: spec
//! section: 28.2
//! title: Dispose through a record receiver has no such member
//! rule: `Dispose()` is not on `Object`; it is class-only — a member of class
//!   types and of the interfaces that are or extend `IDisposable` or declare
//!   it (below), and of nothing else
//! expect: reject
//! diagnostic: Dispose
module Conf;

record Money(Amount: decimal);

public class Program
  public method Demo(m: Money)
    m.Dispose();
  end Demo;
end Program;
