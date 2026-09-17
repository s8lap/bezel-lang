//! case: s11_8_0004
//! doc: spec
//! section: 11.8
//! title: an interface declaring static members may not mark a type parameter variant
//! rule: An interface that declares static members may not mark its type
//!   parameters variant (in/out) -- a compile error at the declaration.
//! cites: 11.8, 12.9
//! expect: reject
//! diagnostic: variant
module Conf;

public interface IBox<out T>
  static method Default(): owned T;
  readonly property Value: T;
end IBox;
