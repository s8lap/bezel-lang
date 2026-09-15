//! case: s22_10_0005
//! doc: spec
//! section: 22.10
//! title: a convenience constructor's this(...) chaining position may read its own parameters
//! rule: The only reads a chaining argument may make are of the
//!   constructor's own parameters, constants, and statics.
//! cites: 22.10, 22.8
//! expect: accept
module Conf;

public class Greeter(name: string, prefix: string)
  private _label: string := prefix + ": ";

  public method Greet(): string => _label + name;

  public constructor(p: string)
    this("default", p);
  end constructor;
end Greeter;
