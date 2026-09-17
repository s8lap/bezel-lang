//! case: s18_8_0001
//! doc: spec
//! section: 18.8
//! title: the \x escape greedily consumes up to 4 hex digits as a single code point
//! rule: "The `\\x` escape greedily consumes the maximum number of valid
//!   hexadecimal digits, up to 4. In the string `\"\\x41BC\"`, the escape is
//!   `\\x41BC` (U+41BC), not `\\x41` followed by `BC`."
//! cites: 18.8
//! expect: run
//! stdout: 1
module Conf;

public class Program
  public static method Main()
    var s := "\x41BC";
    Console.WriteLine(s.Length);
  end Main;
end Program;
