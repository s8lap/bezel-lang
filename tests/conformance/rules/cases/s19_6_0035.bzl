//! case: s19_6_0035
//! doc: spec
//! section: 19.6
//! title: A case expression over DayOfWeek with a wildcard arm runs the matching arm
//! rule: A `case` expression with `_` as its last arm is permitted (§19.6, v0.350); `Monday` (stdlib §2.5) matches its own arm, giving 2, not the wildcard 3.
//! cites: 19.6, 2.5
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    var d: DayOfWeek := DayOfWeek.Monday;
    var s := (case d of DayOfWeek.Sunday => 1, DayOfWeek.Monday => 2, _ => 3);
    Console.WriteLine(s);
  end Main;
end Program;
