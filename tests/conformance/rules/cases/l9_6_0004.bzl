//! case: l9_6_0004
//! doc: stdlib
//! section: 9.6
//! title: PadCenter puts the odd extra padding character on the right
//! rule: PadCenter splits the padding evenly; when it is odd, the extra
//!   character goes to the RIGHT: "ab".PadCenter(5) = " ab  ".
//! cites: 9.6
//! expect: run
//! stdout: [ ab  ]
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("[" + "ab".PadCenter(5) + "]");
  end Main;
end Program;
