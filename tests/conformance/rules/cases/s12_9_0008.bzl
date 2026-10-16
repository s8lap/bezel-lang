//! case: s12_9_0008
//! doc: spec
//! section: 12.9
//! title: a covariant type parameter in a function type's return, at a method parameter, is rejected
//! rule: Function types are themselves variant — covariant in return type,
//!   contravariant in parameter types — and the same composition rule applies,
//!   so T inside `() -> T` at a method parameter composes to contravariant.
//! cites: 12.9
//! expect: reject
//! diagnostic: position
//! issue: 6348
module Conf;

public interface I<out T>
  method Register(f: () -> T);
end I;
