//! case: s22_10_0006
//! doc: spec
//! section: 22.10
//! title: ref is rejected on a primary constructor parameter
//! rule: A primary-constructor parameter list admits plain and `owned`
//!   parameters only; `ref` and `out` are rejected semantically.
//! cites: 22.10
//! expect: reject
//! diagnostic: ref
module Conf;

public class Box(ref value: int)
  public method Value(): int => value;
end Box;
