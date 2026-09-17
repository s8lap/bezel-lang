//! case: s22_10_0004
//! doc: spec
//! section: 22.10
//! title: a convenience constructor's this(...) chaining position may not read a primary-constructor parameter
//! rule: Primary-constructor parameters are not in scope in a convenience
//!   constructor's chaining position: the arguments of a `this(...)` call
//!   run before the chained-to primary's capture step, so reading a
//!   primary-constructor parameter there is a compile error.
//! cites: 22.10, 22.8
//! expect: reject
//! diagnostic: not in scope
module Conf;

public class Greeter(name: string, prefix: string)
  private _label: string := prefix + ": ";

  public method Greet(): string => _label + name;

  public constructor()
    this(name, "hi");
  end constructor;
end Greeter;
