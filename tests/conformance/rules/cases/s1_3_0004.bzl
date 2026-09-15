//! case: s1_3_0004
//! doc: spec
//! section: 1.3
//! title: the bare underscore cannot be declared as a local's name
//! rule: The bare underscore `_` is not an identifier — never declarable and
//!   never readable as a name; `var _ := e;` is a compile error.
//! cites: 1.3, 17.13, C.1
//! expect: reject
//! diagnostic: discard
module Conf;

public class Holder
  private method Use()
    var _ := 5;
  end Use;
end Holder;
