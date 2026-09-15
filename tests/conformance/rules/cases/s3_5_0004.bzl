//! case: s3_5_0004
//! doc: spec
//! section: 3.5
//! title: bitwise operators on int operands compile
//! rule: Bitwise (&, |, ^, ~) on byte, sbyte, short, ushort, int, uint, long, ulong.
//! cites: 3.5
//! expect: accept
module Conf;

public class Checker
  public static method Combine(): int => 1 & 2;
end Checker;
