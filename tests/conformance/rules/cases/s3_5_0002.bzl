//! case: s3_5_0002
//! doc: spec
//! section: 3.5
//! title: the ordering operators on int operands compile
//! rule: Ordering (<, <=, >, >=) on every primitive except bool.
//! cites: 3.5
//! expect: accept
module Conf;

public class Checker
  public static method Compare(): bool => 1 < 2;
end Checker;
