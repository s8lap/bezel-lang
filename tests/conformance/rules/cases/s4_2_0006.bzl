//! case: s4_2_0006
//! doc: spec
//! section: 4.2
//! title: the boundary carve-out admits int.MinValue's magnitude as unary minus's immediate operand
//! rule: The decimal literal of magnitude 2^31 (unsuffixed) is admitted only
//!   as the immediate operand of unary -, denoting int.MinValue.
//! cites: 4.2
//! expect: run
//! stdout: -2147483648
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{-2147483648}");
  end Main;
end Program;
