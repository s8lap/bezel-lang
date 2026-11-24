//! case: s14_2_8114
//! doc: spec
//! section: 14.2
//! title: an alias of the stdlib record DateTime does not satisfy where T: class
//! rule: `where T: class` | Non-nullable reference types only
//! cites: 14.2, 27.8
//! expect: reject
//! diagnostic: class
//! notes: Pins #9033's class arm for a second stdlib record (stdlib: `public record
//!  DateTime`).
module Conf;
type DT := DateTime;
public class P
  public static method F<T>(x: T): int where T: class => 1;
  public static method Main(): int
    var v: DT := DateTime.FromTicks(5L);
    Console.WriteLine(P.F(v));
    return 0;
  end Main;
end P;
