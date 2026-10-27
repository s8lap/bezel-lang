//! case: l9_6_0012
//! doc: stdlib
//! section: 9.6
//! title: PadCenter splits an even padding evenly
//! rule: PadCenter splits the padding evenly; when it is odd, the extra
//!   character goes to the RIGHT: "ab".PadCenter(5) = " ab  ".
//! cites: 9.6
//! expect: run
//! stdout: [  ab  ]
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("[" + "ab".PadCenter(6) + "]");
  end Main;
end Program;
