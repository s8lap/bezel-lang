//! case: s3_5_0001
//! doc: spec
//! section: 3.5
//! title: the ordering operators on bool operands are a compile error
//! rule: Ordering (<, <=, >, >=) on every primitive except bool — the
//!   ordering operators on bool operands are a compile error.
//! cites: 3.5
//! expect: reject
//! diagnostic: ordering
module Conf;

public class Checker
  public static method Compare(): bool => true < false;
end Checker;
