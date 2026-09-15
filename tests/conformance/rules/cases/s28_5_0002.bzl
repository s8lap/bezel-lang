//! case: s28_5_0002
//! doc: spec
//! section: 28.5
//! title: owned on a string field is rejected
//! rule: The declared type of an `owned` slot, nullability stripped, must be a class
//!   type, an interface type, or a bare type parameter. On `string` — a
//!   runtime-managed type — `owned` is a compile error.
//! cites: 28.1, 28.5, 28.9
//! expect: reject
//! diagnostic: owned
module Conf;

public class Holder
  private owned _name: string := "conf";
end Holder;
