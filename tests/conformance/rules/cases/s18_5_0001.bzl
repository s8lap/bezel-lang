//! case: s18_5_0001
//! doc: spec
//! section: 18.5
//! title: a null interpolation hole renders the four characters null
//! rule: An interpolation hole renders `ToString()`; a null hole renders the four
//!   characters `null`.
//! cites: 18.5
//! expect: run
//! stdout: [null]
module Conf;

public class Program
  public static method Main()
    var name: string? := null;
    Console.WriteLine($"[{name}]");
  end Main;
end Program;
