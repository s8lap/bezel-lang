//! case: s14_2_8102
//! doc: spec
//! section: 14.2
//! title: a stdlib record TimeSpan satisfies where T: value at an explicit call site
//! rule: `where T: value` | Non-nullable value types — records, enums, choices
//! cites: 14.2
//! expect: run
//! output: 1
//! notes: Pins #8733, explicit type-argument spelling of s14_2_8101.
module Conf;
public class P
  public static method F<T>(x: T): int where T: value
    return 1;
  end F;
  public static method Main(): int
    Console.WriteLine(P.F<TimeSpan>(TimeSpan.FromTicks(5L)));
    return 0;
  end Main;
end P;
