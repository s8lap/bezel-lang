//! case: s14_2_0029
//! doc: spec
//! section: 14.2
//! title: where T notnull is the admitted spelling of what an Object bound meant
//! rule: A bound naming `Object` (§8.1) is a compile error at the declaration:
//!   write an unconstrained `T`, or `where T: notnull`.
//! cites: 14.2, 8.1
//! expect: accept
module Conf;

public class Util
  public static method Count<T>(x: T): int where T: notnull => 1;
end Util;
