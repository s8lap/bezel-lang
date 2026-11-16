//! case: l1_0004
//! doc: stdlib
//! section: 1
//! title: a zero constructor magnitude is valid, not a fault
//! rule: Negative magnitudes raise … Zero is valid (an empty container or
//!   zero-length buffer): a magnitude-taking constructor such as
//!   StringBuilder(capacity) constructs successfully at capacity zero, and the
//!   instance is fully usable afterward.
//! cites: 2.1
//! expect: run
//! stdout: x
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder(0);
    sb.Append("x");
    Console.WriteLine(sb.ToString());
  end Main;
end Program;
