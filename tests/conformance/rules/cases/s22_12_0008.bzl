//! case: s22_12_0008
//! doc: spec
//! section: 22.12
//! title: a mutable static field of a primitive value type warns
//! rule: A mutable static whose declared type is a primitive value type
//!   or an enum type ... warns (iii): the obligation is to confine every
//!   use of the storage to one domain, or take the synchronized spelling.
//! cites: 22.12
//! expect: warn
//! diagnostic: one domain
module Conf;

public class Registry
  private static _hits: int := 0;
end Registry;
