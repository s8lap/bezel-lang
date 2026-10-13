//! case: s28_5_0014
//! doc: spec
//! section: 28.5
//! title: a generic owned field instantiated at a runtime-managed type is rejected
//! rule: When a generic class declares `private owned _slot: T` (where T is a
//!   type parameter), the restrictions above are checked at each
//!   instantiation, reading the substituted type as they read a declared
//!   one ... every other substitution — `string`, `Type`, a function type,
//!   a copyable value type, and an owning inline array ... — is a compile
//!   error at the instantiation site
//! cites: 14.2, 28.5
//! expect: reject
//! diagnostic: instantiation
module Conf;

public class Box<T> where T: class
  private owned _slot: T;

  public constructor(owned slot: T)
    _slot := slot;
  end constructor;
end Box;

public class Program
  public method Demo()
    using var b := new Box<string>("conf");
  end Demo;
end Program;
