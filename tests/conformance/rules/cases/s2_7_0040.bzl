//! case: s2_7_0040
//! doc: spec
//! section: 2.7
//! title: A user class named DateTime shadows the always-available DateTime for member lookup through a chain
//! rule: Every always-available name other than the first row may be shadowed by local declarations (spec 2.3, 2.7 rule 1), so a chain off a user DateTime resolves members on the user class, not on the stdlib type.
//! cites: 2.3, 2.7
//! expect: run
//! output: mine
//!   7
//!   3
//!   9
module Conf;

public class DateTime
  public Year: int := 7;
  public DayOfWeek: string := "mine";
  public Kind: int := 3;
  public method AddDays(n: int): DateTime => this;
  public method Foo(): int => 9;
end DateTime;

public class Program
  public static method Main()
    var d := new DateTime();
    var s: string := d.AddDays(3).DayOfWeek;
    var y: int := d.AddDays(3).Year;
    var k: int := d.AddDays(3).Kind;
    Console.WriteLine(s); Console.WriteLine(y); Console.WriteLine(k);
    Console.WriteLine(d.AddDays(1).Foo());
  end Main;
end Program;
