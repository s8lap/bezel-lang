//! case: s28_5_0014
//! doc: spec
//! section: 28.5
//! title: a generic owned field instantiated at a runtime-managed type is rejected
//! rule: When a generic class declares `private owned _slot: T`, the
//!   restriction "only fields of class or interface types may be `owned`" is
//!   checked at instantiation. Instantiating with a type that satisfies
//!   `where T: class` but is neither a class nor an interface (`string`,
//!   function types) is a compile error at the instantiation site.
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
    var b: Box<string>;
  end Demo;
end Program;
