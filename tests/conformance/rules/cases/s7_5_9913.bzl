//! case: s7_5_9913
//! doc: spec
//! section: 7.5
//! title: A null receiver skips the extension call and its argument evaluation
//! rule: no member is read, no call is performed, and the argument and index expressions of the skipped
//!   suffixes are not evaluated either
//! cites: 7.5, 26.5
//! expect: run
//! output: x3
//!   lit3
//!   1
//!   cond3
//!   3
//! notes: Harvested from the #9243 review (r43-rev-m146-9243). A value-shaped receiver (an int or an enum)
//!   is non-null here, so `x?.M` performs the access, and an extension member is found by 26.5 rule 2
//!   exactly as for `.`. The result type stays nullable (7.5). The record-receiver and Console.Out
//!   residuals of #9243 are not covered here. Statement form performs the call (x3, lit3); the null
//!   int? z evaluates no argument, so Arg ran once (1); the result form prints cond3 then 3.
module Conf;

public extension int
  public method Say(m: string): int
    Console.WriteLine(m + this.ToString());
    return this;
  end Say;
end;

public class Program
  public static calls: int := 0;

  public static method Arg(): string
    Program.calls := Program.calls + 1;
    return "x";
  end Arg;

  public static method Main()
    var x: int := 3;
    x?.Say(Program.Arg());
    x?.Say("lit");
    var z: int? := null;
    z?.Say(Program.Arg());
    Console.WriteLine(Program.calls.ToString());
    var r := x?.Say("cond");
    Console.WriteLine((r ?? -1).ToString());
  end Main;
end Program;
