//! case: s4_6_0018
//! doc: spec
//! section: 4.6
//! title: IsAscii is true iff every code point of the string is below 128
//! rule: Language spec §4.6 (v0.345): "A pure-ASCII string (every code point in U+0000–U+007F)
//!   is identical in memory layout to a UTF-8 byte sequence. Implementations expose this through
//!   the runtime predicate `s.IsAscii: bool`" (standard library §9.6). A string holding a
//!   non-ASCII code point is therefore not ASCII.
//! cites: 4.6, 9.6
//! expect: run
//! stdout: False
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("日".IsAscii);
  end Main;
end Program;
