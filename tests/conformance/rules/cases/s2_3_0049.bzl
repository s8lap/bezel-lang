//! case: s2_3_0049
//! doc: spec
//! section: 2.3
//! title: Shadowing DateTimeKind by a class leaves the System DayOfWeek reachable
//! rule: Language spec §2.3 (v0.350): the auto-imported `System` names, DayOfWeek and DateTimeKind (stdlib §2.5) among them, "may be shadowed by local declarations under the normal name-resolution rules". The shadow is per name: a user class `DateTimeKind` hides only that name, and `DayOfWeek.Friday` still names the System enum constant (stdlib §2.5: Sunday is 0, so Friday is 5).
//! cites: 2.3, 2.5
//! expect: run
//! output: 7
//!   5
module Conf;

public class DateTimeKind
  public static method Hi(): int return 7; end Hi;
end DateTimeKind;

public class Program
  public static method Main()
    Console.WriteLine(DateTimeKind.Hi());
    var w: DayOfWeek := DayOfWeek.Friday;
    Console.WriteLine(int(byte(w)));
  end Main;
end Program;
