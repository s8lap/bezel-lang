//! case: s14_2_8111
//! doc: spec
//! section: 14.2
//! title: an alias of a stdlib record does not satisfy where T: class at a call site
//! rule: `where T: class` | Non-nullable reference types only
//! cites: 14.2, 27.8
//! expect: reject
//! diagnostic: class
//! notes: Pins #9033's class arm. An alias is a pure synonym (27.8), so TS is the record
//!  TimeSpan, a value type, not a reference type. Twin: s14_2_8115 (the same alias under
//!  where T: value).
module Conf;
type TS := TimeSpan;
public class P
  public static method F<T>(x: T): int where T: class => 1;
  public static method Main(): int
    var v: TS := TimeSpan.FromTicks(5L);
    Console.WriteLine(P.F(v));
    return 0;
  end Main;
end P;
