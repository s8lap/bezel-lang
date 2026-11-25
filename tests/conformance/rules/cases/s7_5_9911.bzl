//! case: s7_5_9911
//! doc: spec
//! section: 7.5
//! title: A chain of ?. extension calls over an int stops at a null int?
//! rule: If any segment of a chain is null, the remainder of the chain is skipped on the same terms and
//!   the whole chain's result is `null`
//! cites: 7.5, 26.5
//! expect: run
//! output: 20
//!   12
//!   -1
//!   99
//! notes: Harvested from the #9243 review (r43-rev-m146-9243). A value-shaped receiver (an int or an enum)
//!   is non-null here, so `x?.M` performs the access, and an extension member is found by 26.5 rule 2
//!   exactly as for `.`. The result type stays nullable (7.5). The record-receiver and Console.Out
//!   residuals of #9243 are not covered here. 5 -> 10 -> 20; 3 -> 6 -> 12; the null int?
//!   short-circuits to -1 and to the fallback 99.
module Conf;

public extension int
  public method Dbl(): int => this * 2;
end;

public class Program
  public static method Main()
    var x: int := 5;
    var a := x?.Dbl()?.Dbl();
    Console.WriteLine((a ?? -1).ToString());
    var y: int? := 3;
    var b := y?.Dbl()?.Dbl();
    Console.WriteLine((b ?? -1).ToString());
    var z: int? := null;
    var c := z?.Dbl()?.Dbl();
    Console.WriteLine((c ?? -1).ToString());
    var d := (z?.Dbl()) ?? 99;
    Console.WriteLine(d.ToString());
  end Main;
end Program;
