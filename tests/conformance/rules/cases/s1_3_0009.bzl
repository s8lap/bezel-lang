//! case: s1_3_0009
//! doc: spec
//! section: 1.3
//! title: reading an ordinary local in expression position is legal
//! rule: A bare underscore cannot be read as a value; an ordinary literal
//!   read in the same position is unaffected.
//! expect: accept
module Conf;

public class Holder
  private method Use()
    var y := 5;
  end Use;
end Holder;
