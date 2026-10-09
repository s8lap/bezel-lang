//! case: s28_14_0025
//! doc: spec
//! section: 28.14
//! title: a method reference over a generic ref parameter at a function-type instantiation is rejected
//! rule: Forming a method reference is likewise a compile error when a parameter
//!   slot of the formed function type is, nullability stripped, a function
//!   type and binds to a callee parameter, other than an `out` parameter,
//!   that is a set-erasing position (... `var put: (ref () -> int) -> () :=
//!   b.Put<() -> int>;` over `Put<T>(ref x: T)`): a call through the formed
//!   value passes
//!   its argument to a plain or `ref` function-typed parameter
//! cites: 28.14, 28.3, 27.6
//! expect: reject
//! diagnostic: method reference
//! issue: 5515
module Conf;

public class Bin
  public constructor()
  end constructor;

  public method Put<T>(ref x: T)
  end Put;
end Bin;

public class Client
  public static method Form()
    using var b := new Bin();
    var put: (ref () -> int) -> () := b.Put<() -> int>;
  end Form;
end Client;
