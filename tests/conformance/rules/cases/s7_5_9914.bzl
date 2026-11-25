//! case: s7_5_9914
//! doc: spec
//! section: 7.5
//! title: The ?. spelling performs a generic extension method over an int receiver
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 26.5, 26.4
//! expect: run
//! output: a
//!   4
//! notes: Harvested from the #9243 review (r43-rev-m146-9243). A value-shaped receiver (an int or an enum)
//!   is non-null here, so `x?.M` performs the access, and an extension member is found by 26.5 rule 2
//!   exactly as for `.`. The result type stays nullable (7.5). The record-receiver and Console.Out
//!   residuals of #9243 are not covered here. Pick returns its first argument: a, then 4.
module Conf;

public extension int
  public method Pick<T>(a: T, b: T): T => a;
end;

public class Program
  public static method Main()
    var x: int := 7;
    var r := x?.Pick<string>("a", "b");
    Console.WriteLine(r ?? "null");
    var r2 := x?.Pick(4, 5);
    Console.WriteLine((r2 ?? -1).ToString());
  end Main;
end Program;
