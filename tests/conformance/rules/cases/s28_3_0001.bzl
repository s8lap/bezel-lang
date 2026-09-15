//! case: s28_3_0001
//! doc: spec
//! section: 28.3
//! title: a static method returning a plain class type is rejected at the declaration
//! rule: A static method whose declared return type (nullability stripped) is a class
//!   or interface type must announce its classification in the signature — `owned`, a
//!   mint-announcing type, or a bare type parameter with a non-empty source set. Any
//!   other such signature is a compile error at the declaration.
//! cites: 28.3, C.3
//! expect: reject
//! diagnostic: owned
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public class Picker
  public static method Pick(a: Resource, b: Resource): Resource => a;
end Picker;
