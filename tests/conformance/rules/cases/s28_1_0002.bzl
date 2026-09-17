//! case: s28_1_0002
//! doc: spec
//! section: 28.1
//! title: owned on a method parameter of primitive type is rejected
//! rule: The declared type of an `owned` parameter, nullability stripped, must be a
//!   class type, an interface type, or a bare type parameter; a primitive is a
//!   compile error.
//! expect: reject
//! diagnostic: owned
module Conf;

public class Pool
  public method Adopt(owned n: int)
  end Adopt;
end Pool;
