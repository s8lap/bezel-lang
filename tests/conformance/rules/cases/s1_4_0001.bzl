//! case: s1_4_0001
//! doc: spec
//! section: 1.4
//! title: a reserved keyword cannot be used as an identifier
//! rule: Reserved words may not be used as identifiers, except for the
//!   contextual keywords listed in Appendix B which have meaning only in
//!   specific syntactic positions.
//! cites: 1.4
//! expect: reject
//! diagnostic: reserved
module Conf;

public class Holder
  private method Use()
    var move := 5;
  end Use;
end Holder;
