//! case: s1_3_0007
//! doc: spec
//! section: 1.3
//! title: a named parameter is an ordinary declaration
//! rule: An identifier merely containing an underscore is ordinary; a
//!   properly named parameter is unaffected by the discard rule.
//! expect: accept
module Conf;

public class Holder
  private method Use(x: int)
  end Use;
end Holder;
