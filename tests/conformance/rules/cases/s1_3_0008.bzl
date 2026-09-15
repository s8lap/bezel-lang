//! case: s1_3_0008
//! doc: spec
//! section: 1.3
//! title: a bare underscore cannot be read as a value
//! rule: The bare underscore is never readable as a name; a bare `_` in
//!   expression position is a compile error — `_` is not a value, so no such
//!   read can resolve (§17.13).
//! cites: 1.3, 17.13, C.1
//! expect: reject
//! diagnostic: discard
module Conf;

public class Holder
  private method Use()
    var y := _;
  end Use;
end Holder;
