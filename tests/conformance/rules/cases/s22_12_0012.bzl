//! case: s22_12_0012
//! doc: spec
//! section: 22.12
//! title: a mutable static field of bare generic parameter type is rejected at the declaration
//! rule: A mutable static field or auto-property whose declared type is a
//!   bare generic parameter, or its nullable form, is admitted by no form
//!   above and is a compile error at the declaration site, whatever the
//!   parameter's constraints or eventual substitutions.
//! cites: 22.12
//! expect: reject
//! diagnostic: generic parameter
module Conf;

public class Box<T>
  private static _slot: T;
end Box;
