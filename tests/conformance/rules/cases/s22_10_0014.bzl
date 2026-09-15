//! case: s22_10_0014
//! doc: spec
//! section: 22.10
//! title: a convenience constructor chains to the primary via this(...)
//! rule: A class with a primary constructor may declare additional
//!   convenience constructors. Each must chain to another constructor via
//!   `this(...)`; only the primary may chain to `base(...)`.
//! cites: 22.10, 22.7
//! expect: accept
module Conf;

public class Base
  public constructor()
  end constructor;
end Base;

public class Derived(name: string): Base()
  private _name: string := name;

  public constructor() this("default") end constructor;
end Derived;
