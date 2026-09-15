//! case: s14_2_0012
//! doc: spec
//! section: 14.2
//! title: an interface never satisfies where T: new()
//! rule: Interface substitutions never satisfy `new()` — no constructor
//!   exists (§14.2).
//! cites: 14.2
//! expect: reject
//! diagnostic: new()
module Conf;

public interface IFoo
  method Ping();
end IFoo;

public static class Factory
  public static method Make<T>(): owned T where T: new() => new T();
end Factory;

public class Program
  public static method Main()
    var f := Factory.Make<IFoo>();
  end Main;
end Program;
