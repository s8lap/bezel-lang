//! case: s1_6_0002
//! doc: spec
//! section: 1.6
//! title: a method's end label that mismatches its name is rejected
//! rule: The optional label is the identifier: `end methodName;`. A mismatch
//!   is a compile error.
//! cites: 1.6, C.1
//! expect: reject
//! diagnostic: match
module Conf;

public class Holder
  private method Compute(): int
    return 1;
  end Wrong;
end Holder;
