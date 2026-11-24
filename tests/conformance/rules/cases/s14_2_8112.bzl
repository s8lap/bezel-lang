//! case: s14_2_8112
//! doc: spec
//! section: 14.2
//! title: an alias of an alias of a stdlib record does not satisfy where T: class at a call
//!  site
//! rule: `where T: class` | Non-nullable reference types only
//! cites: 14.2, 27.8
//! expect: reject
//! diagnostic: class
//! notes: Pins #9033's class arm through a chain of aliases (27.8: a pure synonym at every
//!  link).
module Conf;
type TS := TimeSpan;
type TS2 := TS;
public class P
  public static method F<T>(x: T): int where T: class => 1;
  public static method Main(): int
    var v: TS2 := TimeSpan.FromTicks(5L);
    Console.WriteLine(P.F(v));
    return 0;
  end Main;
end P;
