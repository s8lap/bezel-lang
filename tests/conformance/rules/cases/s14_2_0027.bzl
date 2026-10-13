//! case: s14_2_0027
//! doc: spec
//! section: 14.2
//! title: a class type bound naming Object is rejected at the declaration
//! rule: A bound naming `Object` (§8.1) is a compile error at the declaration:
//!   write an unconstrained `T`, or `where T: notnull`.
//! cites: 14.2, 8.1
//! expect: reject
//! diagnostic: Object
//! issue: 5805
module Conf;

public class Keep<T> where T: Object
  public constructor()
  end constructor;
end Keep;
