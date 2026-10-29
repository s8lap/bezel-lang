//! case: s7_1_0004
//! doc: spec
//! section: 7.1
//! title: the same parse call with a non-null string argument is accepted and runs
//! rule: Language spec §7.1 (v0.350): "The compiler enforces the distinction at every assignment,
//!   parameter pass, and return." The parameter admits a string, and stdlib §9.1 (v0.350) declares
//!   `public static pure method Parse(s: string): int;`.
//! cites: 7.1, 9.1
//! expect: run
//! stdout: 7
//! notes: the accepting twin of s7_1_0003: the one edit is the null literal becoming "7".
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("7"));
  end Main;
end Program;
