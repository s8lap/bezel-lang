//! case: l2_18_0026
//! doc: stdlib
//! section: 2.18
//! title: the no-format ToString of a double is the shortest string that parses back
//! rule: stdlib §2.18's floating row: "`R` and the **no-format `ToString()`**
//!     both produce the shortest string that parses back to the identical value
//!     — of several such, the one nearest it (round-trip guarantee)". One third
//!     renders `0.3333333333333333` and one tenth renders `0.1`.
//! cites: 2.18
//! expect: run
//! output: 0.3333333333333333
//!   0.1
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((1.0 / 3.0).ToString());
    Console.WriteLine((0.1).ToString());
  end Main;
end Program;
