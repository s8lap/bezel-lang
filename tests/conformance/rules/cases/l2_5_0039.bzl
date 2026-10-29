//! case: l2_5_0039
//! doc: stdlib
//! section: 2.5
//! title: DateTime.DayOfWeek chained off every receiver shape yields the day of that date
//! rule: DateTime.DayOfWeek is a property of type DayOfWeek, an enum over byte with Sunday = 0 through Saturday = 6 (stdlib 2.5); 2024-03-15 is a Friday, value 5. The property read applies to any DateTime-typed receiver expression: field, property, call result, chained call, index, loop variable, record field, lambda parameter, out variable, generic field, narrowed nullable and map value.
//! cites: 2.5, 5.4
//! expect: run
//! output: 5
//!   5
//!   5
//!   6
//!   6
//!   5
//!   6
//!   1
//!   2
//!   2
//!   3
//!   0
//!   4
//!   5
//!   6
//!   0
//!   1
//!   1
//!   1
//!   2
module Conf;

extension DateTime
  public method Dow(): DayOfWeek => this.DayOfWeek;
  public method DowNext(): DayOfWeek => this.AddDays(1).DayOfWeek;
end;

public record Ev(When: DateTime, Name: string);

public class Cal
  public Start: DateTime := new DateTime(2024, 3, 15);
  public property First: DateTime => Start;
  public method Get(): DateTime => Start;
end Cal;

public class Gen<T>
  public Item: T;
  public constructor(i: T) Item := i; end constructor;
end Gen;

public class Program
  public static method Take(d: DayOfWeek): int => int(d);
  public static method Ident<T>(x: T): T => x;
  public static method Fill(out d: DateTime) d := new DateTime(2024, 3, 17); end Fill;
  public static method Main()
    var c := new Cal();
    Console.WriteLine(Take(c.Start.DayOfWeek));
    Console.WriteLine(Take(c.First.DayOfWeek));
    Console.WriteLine(Take(c.Get().DayOfWeek));
    Console.WriteLine(Take(c.Start.AddDays(1).DayOfWeek));
    var arr: Array<DateTime> := [new DateTime(2024, 3, 15), new DateTime(2024, 3, 16)];
    Console.WriteLine(Take(arr[1].DayOfWeek));
    for var x in arr do Console.WriteLine(Take(x.DayOfWeek)); end;
    var ev := new Ev(new DateTime(2024, 3, 18), "e");
    Console.WriteLine(Take(ev.When.DayOfWeek));
    Console.WriteLine(Take(ev.When.AddDays(1).DayOfWeek));
    var f := lambda(x: DateTime) => Take(x.DayOfWeek);
    Console.WriteLine(f(new DateTime(2024, 3, 19)));
    var g := lambda(x: DateTime) => x.DayOfWeek;
    Console.WriteLine(int(g(new DateTime(2024, 3, 20))));
    var dd: DateTime := new DateTime(2000, 1, 1);
    Fill(out dd);
    Console.WriteLine(Take(dd.DayOfWeek));
    var gen := new Gen<DateTime>(new DateTime(2024, 3, 21));
    Console.WriteLine(Take(gen.Item.DayOfWeek));
    Console.WriteLine(Take(Ident(new DateTime(2024, 3, 22)).DayOfWeek));
    Console.WriteLine(Take(new DateTime(2024, 3, 23).DayOfWeek));
    Console.WriteLine(int(new DateTime(2024, 3, 24).Dow()));
    Console.WriteLine(int(new DateTime(2024, 3, 24).DowNext()));
    var n: DateTime? := new DateTime(2024, 3, 25);
    if n <> null then Console.WriteLine(Take(n.DayOfWeek)); end;
    Console.WriteLine(Take(n!.DayOfWeek));
    var dict := new Dictionary<string, DateTime>();
    dict["a"] := new DateTime(2024, 3, 26);
    Console.WriteLine(Take(dict["a"].DayOfWeek));
  end Main;
end Program;
