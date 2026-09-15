//! case: s10_3_0001
//! doc: spec
//! section: 10.3
//! title: a colon-list head outside the eight integer types is read as an interface, and rejected as one
//! rule: The colon list is one list: if its first item resolves to one of
//!   the eight underlying-type keywords, that item is the underlying type;
//!   otherwise the underlying type is `int` and every item names an
//!   interface. `double` resolves to none of the eight, so it is read as an
//!   attempted interface — and is not one.
//! cites: 10.3
//! expect: reject
//! diagnostic: interface
module Conf;

enum Ratio: double
  Half, Whole;
end Ratio;
