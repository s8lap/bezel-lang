//! case: s2_3_0048
//! doc: spec
//! section: 2.3
//! title: A user enum named DayOfWeek shadows the System one
//! rule: Language spec §2.3 (v0.350): the auto-imported `System` names, DayOfWeek and DateTimeKind (stdlib §2.5) among them, "may be shadowed by local declarations under the normal name-resolution rules". A module that declares `enum DayOfWeek: byte Alpha, Beta` therefore resolves `DayOfWeek.Beta` to its own constant; its underlying value is 1 and `nameof(d)` is the identifier "d".
//! cites: 2.3, 10.1
//! expect: run
//! output: d
//!   1
module Conf;

public enum DayOfWeek: byte Alpha, Beta; end DayOfWeek;

public class Program
  public static method Main()
    var d: DayOfWeek := DayOfWeek.Beta;
    Console.WriteLine(nameof(d));
    Console.WriteLine(int(byte(d)));
  end Main;
end Program;
