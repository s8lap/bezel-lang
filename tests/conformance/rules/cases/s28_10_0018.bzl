//! case: s28_10_0018
//! doc: spec
//! section: 28.10
//! title: using var over a value-type right-hand side disposes nothing at scope end
//! rule: `using var` accepts a value-type or runtime-managed right-hand side. For
//!   ordinary value types and runtime-managed types, the disposal at scope end
//!   is a no-op (these types do not require disposal per
//!   [§28.9](#sec-runtime-managed-types)), and the only effect is the
//!   reassignability restriction.
//! cites: 28.10, 28.9
//! expect: run
//! output: [5]

module Conf;

public class Program
  public static method Main()
    using var x := 5;
    Console.WriteLine($"[{x}]");
  end Main;
end Program;
