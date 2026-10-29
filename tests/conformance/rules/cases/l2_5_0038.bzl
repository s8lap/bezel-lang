//! case: l2_5_0038
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan has no Duration member and the call is refused
//! rule: stdlib §2.5 declares no `Duration` on TimeSpan (the members are those listed there); a member the type does not declare is a compile error (§22.1).
//! cites: 2.5, 22.1
//! expect: reject
//! diagnostic: Duration
module Conf;

public class Program
  public static method Main()
    var a := TimeSpan.FromTicks(-5L);
    var d := a.Duration();
    Console.WriteLine(d.Ticks);
  end Main;
end Program;
