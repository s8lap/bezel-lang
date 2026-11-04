//! case: s3_4_0008
//! doc: spec
//! section: 3.4
//! title: String.Repeat and string.Repeat give the same result
//! rule: Static members of the standard library type are accessible through either
//!   form
//! cites: 3.4, 9.6
//! expect: run
//! output: abab
//!   abab
//! issue: 7494
//! notes: twin of s3_4_0009 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(String.Repeat("ab", 2));
    Console.WriteLine(string.Repeat("ab", 2));
  end Main;
end Program;
