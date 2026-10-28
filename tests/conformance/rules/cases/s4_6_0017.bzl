//! case: s4_6_0017
//! doc: spec
//! section: 4.6
//! title: a string's Length is its number of code points, not its byte count
//! rule: Language spec §4.6 (v0.345): "`s.Length` returns the number of code points in `s`.
//!   O(1)." The user-visible model is "a sequence of code points indexed in O(1)", so a string
//!   holding one two-byte code point beside one ASCII character has length 2.
//! cites: 4.6
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    var s := "a" + "日";
    Console.WriteLine(s.Length);
  end Main;
end Program;
