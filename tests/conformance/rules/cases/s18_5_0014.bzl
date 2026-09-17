//! case: s18_5_0014
//! doc: spec
//! section: 18.5
//! title: a doubled brace in an interpolated string renders as a single literal brace
//! rule: "In interpolated strings ... a doubled brace denotes a literal brace:
//!   `{{` produces `{`, `}}` produces `}`."
//! cites: 18.5
//! expect: run
//! stdout: {literal braces}
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{{literal braces}}");
  end Main;
end Program;
