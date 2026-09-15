//! case: s30_10_0011
//! doc: spec
//! section: 30.10
//! title: Task.FromResult at a class-typed T is rejected
//! rule: `Task.FromResult<T>(value: T): Task<T>` is restricted to non-class T;
//!   calling FromResult with a class-typed T is a compile error pointing to
//!   the owned variant.
//! cites: 30.10
//! expect: reject
//! diagnostic: FromOwnedResult
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Describe(): string
    return "gear";
  end Describe;
end Widget;

public class Program
  public static method Demo()
    using var w := new Widget();
    using var t := Task.FromResult(w);
  end Demo;
end Program;
