//! case: s26_4_0001
//! doc: spec
//! section: 26.4
//! title: a bare identifier in the extended type's argument position binds a parameter
//! rule: In the extended type's argument positions, each bare single-segment
//!   non-nullable identifier introduces a fresh block-level type parameter.
//! cites: 26.4
//! expect: accept
module Conf;

extension Array<T>
  public method CountTwice(): int => this.Count * 2;
end;
