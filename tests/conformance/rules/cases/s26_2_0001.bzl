//! case: s26_2_0001
//! doc: spec
//! section: 26.2
//! title: a primitive type may be extended
//! rule: Classes, interfaces, records, enums and built-in primitive types may be
//!   extended.
//! cites: 26.2
//! expect: accept
module Conf;

extension int
  public property IsPositive: bool => this > 0;
end;
