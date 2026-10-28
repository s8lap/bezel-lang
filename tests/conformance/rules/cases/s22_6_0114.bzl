//! case: s22_6_0114
//! doc: spec
//! section: 22.6
//! title: char.Parse of an int argument reaches no candidate
//! rule: stdlib §9.5 declares `public static pure method Parse(s: string):
//!     char;` — a `string` parameter and no other. Language spec §22.6: "If no
//!     candidate is applicable at all — no candidate is reachable: a conversion
//!     §5.1 does not admit as implicit ... the call is a compile error with no
//!     candidates cited, because no overload was reachable." An `int` argument
//!     reaches no candidate, so the call is a compile error.
//! cites: 22.6, 9.5, 5.1
//! expect: reject
//! diagnostic: no matching overload
//! issue: 7299
module Conf;

public class Program
  public static method Main()
    var c := char.Parse(16);
    Console.WriteLine(c);
  end Main;
end Program;
