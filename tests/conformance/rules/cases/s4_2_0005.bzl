//! case: s4_2_0005
//! doc: spec
//! section: 4.2
//! title: the same value under the ulong suffix fits
//! rule: A suffixed integer literal whose value fits the suffix's type is
//!   admitted (5000000000U exceeds uint's range — write 5000000000UL).
//! cites: 4.2
//! expect: run
//! stdout: 5000000000
module Conf;

public class Program
  public static method Main()
    var x := 5000000000UL;
    Console.WriteLine($"{x}");
  end Main;
end Program;
