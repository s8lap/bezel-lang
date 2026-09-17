//! case: s1_4_0002
//! doc: spec
//! section: 1.4
//! title: an identifier that merely resembles a keyword is legal
//! rule: Reserved words may not be used as identifiers; a spelling that is
//!   not itself a reserved word is unaffected.
//! expect: accept
module Conf;

public class Holder
  private method Use()
    var mover := 5;
  end Use;
end Holder;
