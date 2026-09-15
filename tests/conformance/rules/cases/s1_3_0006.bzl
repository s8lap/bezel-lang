//! case: s1_3_0006
//! doc: spec
//! section: 1.3
//! title: a parameter cannot be named the bare underscore
//! rule: The bare underscore `_` is never declarable as a name; every
//!   declaration named `_` — including a method parameter — is a compile
//!   error naming the sanctioned forms (§17.13).
//! cites: 1.3, 17.13, C.1
//! expect: reject
//! diagnostic: discard
module Conf;

public class Holder
  private method Use(_: int)
  end Use;
end Holder;
