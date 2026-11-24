//! case: s14_2_8115
//! doc: spec
//! section: 14.2
//! title: an alias of a stdlib record satisfies where T: value at a call site
//! rule: `where T: value` | Non-nullable value types — records, enums, choices
//! cites: 14.2, 27.8
//! expect: run
//! output: 1
//! notes: Pins #9033's value arm; admitting twin of s14_2_8111.
module Conf;
type TS := TimeSpan;
public class P
  public static method F<T>(x: T): int where T: value => 1;
  public static method Main(): int
    var v: TS := TimeSpan.FromTicks(5L);
    Console.WriteLine(P.F(v));
    return 0;
  end Main;
end P;
