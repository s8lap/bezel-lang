//! case: s5_2_0033
//! doc: spec
//! section: 5.2
//! title: a constant operand that fits converts at compile time and is admitted
//! rule: The rule reads the **operand**: a compile-time constant whose value
//!   the conversion admits is not rejected, and the conversion keeps its
//!   ordinary semantics — the rejection reaches only a constant whose value
//!   would raise `OverflowException`.
//! cites: 5.2, 17.14
//! expect: run
//! stdout: 200
//! issue: 4405
module Conf;

public class Program
  public static method Main()
    var b := byte(200);
    Console.WriteLine(b.ToString());
  end Main;
end Program;
