//! case: s22_10_0007
//! doc: spec
//! section: 22.10
//! title: out is rejected on a primary constructor parameter
//! rule: A primary-constructor parameter list admits plain and `owned`
//!   parameters only; `ref` and `out` are rejected semantically.
//! cites: 22.10
//! expect: reject
//! diagnostic: out
module Conf;

public class Box(out value: int)
  public method Value(): int => value;
end Box;
