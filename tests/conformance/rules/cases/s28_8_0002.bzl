//! case: s28_8_0002
//! doc: spec
//! section: 28.8
//! title: implicit boxing of a value used where Object is expected carries no disposal obligation
//! rule: Implicit boxing is not subject to this rule — a boxed value is a
//!   runtime-managed value, not an owned new-result, and Console.WriteLine(42)
//!   does not need to discharge anything.
//! cites: 28.8, 28.9, 8.6
//! expect: run
//! stdout: 42
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(42);
  end Main;
end Program;
