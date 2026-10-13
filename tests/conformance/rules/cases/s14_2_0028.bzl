//! case: s14_2_0028
//! doc: spec
//! section: 14.2
//! title: a method type bound naming Object is rejected at the declaration
//! rule: A bound naming `Object` (§8.1) is a compile error at the declaration:
//!   write an unconstrained `T`, or `where T: notnull`.
//! cites: 14.2, 8.1
//! expect: reject
//! diagnostic: Object
//! issue: 5805
module Conf;

public class Util
  public static method Count<T>(x: T): int where T: Object => 1;
end Util;
