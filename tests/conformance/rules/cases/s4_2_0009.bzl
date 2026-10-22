//! case: s4_2_0009
//! doc: spec
//! section: 4.2
//! title: the same value with an L suffix converts to an explicitly bound double parameter
//! rule: In an untyped context (var x := 3_000_000_000;), and at a non-integer
//!   target (var d: double := 3_000_000_000;), the literal is a compile error
//!   directing to a suffix (3_000_000_000L, which then converts by §5.1).
//! cites: 4.2, 5.1, 14.5
//! expect: accept
module Conf;

public static class Util
  public static method Id<T>(a: T): T => a;
end Util;

public class Program
  public static method Make(): double => Util.Id<double>(3000000000L);
end Program;
