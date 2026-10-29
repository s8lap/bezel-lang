//! case: l9_6_0038
//! doc: stdlib
//! section: 9.6
//! title: string.Repeat over the empty unit is the empty string for a positive count
//! rule: stdlib §9.6 (v0.350) declares `public static pure method Repeat(unit: string, count: int):
//!   string;` and states `Repeat("", n) = ""`, so the empty unit repeated three times has length 0.
//! cites: 9.6
//! expect: run
//! stdout: 0
//! issue: 7129
//! notes: the string overload. The char overload is already pinned by l9_6_0011 and l9_6_0025.
//!   The result's content for a non-empty unit is not stated in prose, so the case observes only
//!   the equation the document does state. Harvested from the R36 reviewer probe repeat_string_arg.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(string.Repeat("", 3).Length);
  end Main;
end Program;
