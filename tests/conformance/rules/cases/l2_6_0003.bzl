//! case: l2_6_0003
//! doc: stdlib
//! section: 2.6
//! title: ParseOrNull returns null for the same malformed string Parse rejects
//! rule: Guid.ParseOrNull(s) returns null exactly where Parse(s) raises;
//!   otherwise the parsed value.
//! cites: 1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Guid.ParseOrNull("not-a-guid") = null);
  end Main;
end Program;
