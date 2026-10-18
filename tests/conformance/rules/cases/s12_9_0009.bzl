//! case: s12_9_0009
//! doc: spec
//! section: 12.9
//! title: the same declaration is admitted under a contravariant marker
//! rule: Function types are themselves variant — covariant in return type,
//!   contravariant in parameter types — and the same composition rule applies,
//!   so T inside `() -> T` at a method parameter composes to contravariant.
//! cites: 12.9
//! expect: accept
//! issue: 6348
module Conf;

public interface I<in T>
  method Register(f: () -> T);
end I;
