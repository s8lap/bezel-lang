//! case: s22_5_0023
//! doc: spec
//! section: 22.5
//! title: a named argument binds the parameter it names after a positional one
//! rule: "Each supplied argument binds **exactly one** parameter and each parameter is bound by **at most one**
//!   supplied argument". Here the positional "a" binds text and width := 2 binds width, so both bindings are unique.
//! cites: 22.5
//! expect: run
//! stdout: a2
//! notes: the legal twin of s22_5_0022, differing only in the name of the second argument.
module Conf;

public class Program
  public static method Pad(text: string := "", width: int := 0): string
    return $"{text}{width}";
  end Pad;

  public static method Main()
    Console.WriteLine(Pad("a", width := 2));
  end Main;
end Program;
