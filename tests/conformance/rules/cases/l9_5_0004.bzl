//! case: l9_5_0004
//! doc: stdlib
//! section: 9.5
//! title: char declares no radix Parse, so a radix call reaches no candidate
//! rule: stdlib §9.5 declares the `char` members — `Parse(s: string): char`,
//!     `TryParse(s: string, out result: char): bool` and
//!     `ParseOrNull(s: string): char?` — and no radix form among them. §22.6:
//!     "If no candidate is applicable at all — no candidate is reachable: a
//!     conversion §5.1 does not admit as implicit ... the call is a compile
//!     error with no candidates cited".
//! cites: 9.5, 22.6
//! expect: reject
//! diagnostic: no matching overload
//! issue: 7295
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(char.Parse("ff", 16));
  end Main;
end Program;
