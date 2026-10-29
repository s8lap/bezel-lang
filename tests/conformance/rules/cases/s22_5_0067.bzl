//! case: s22_5_0067
//! doc: spec
//! section: 22.5
//! title: Path.Combine with reversed or nested named arguments equals the positional call
//! rule: Named arguments may appear in any order after positional arguments
//! cites: 22.5, 7.4
//! expect: run
//! output: True
//!   True
module Conf;

import System.IO;

public class Program
  public static method Main()
    Console.WriteLine(Path.Combine(b := "y", a := "x") = Path.Combine("x", "y"));
    Console.WriteLine(Path.Combine(b := Path.Combine(b := "c", a := "b"), a := "a") = Path.Combine("a", Path.Combine("b", "c")));
  end Main;
end Program;
