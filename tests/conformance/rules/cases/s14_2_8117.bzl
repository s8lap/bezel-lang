//! case: s14_2_8117
//! doc: spec
//! section: 14.2
//! title: an alias of a user class satisfies where T: class at a call site
//! rule: `where T: class` | Non-nullable reference types only
//! cites: 14.2, 27.8
//! expect: run
//! output: 1
//! notes: Boundary of #9033: seeing through an alias must not turn a class alias into a
//!  rejected one under where T: class.
module Conf;
public class Thing
  public constructor() end constructor;
end Thing;
type TH := Thing;
public class P
  public static method F<T>(x: T): int where T: class => 1;
  public static method Main(): int
    using var v: TH := new Thing();
    Console.WriteLine(P.F(v));
    return 0;
  end Main;
end P;
