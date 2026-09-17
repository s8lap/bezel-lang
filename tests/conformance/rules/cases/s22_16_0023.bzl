//! case: s22_16_0023
//! doc: spec
//! section: 22.16
//! title: an invariant in a static class is a compile error
//! rule: Out of scope in this revision, each recorded: ... static-class
//!   and static-state invariants (an `invariant` in a static class is a
//!   compile error — no instance boundary exists).
//! cites: 22.16
//! expect: reject
//! diagnostic: static class
module Conf;

public static class Registry
  invariant(true);
end Registry;
