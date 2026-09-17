//! case: s21_9_0001
//! doc: spec
//! section: 21.9
//! title: a discarded non-unit expression statement result draws a warning
//! rule: An expression statement whose result is a non-`()` type is
//!   permitted but the compiler emits a warning: the result is silently
//!   discarded, which may indicate a bug.
//! cites: 21.9
//! expect: warn
//! diagnostic: unused
module Conf;

public class Program
  public static method ComputeChecksum(): int => 42;

  public static method Main()
    ComputeChecksum();
  end Main;
end Program;
