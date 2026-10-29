//! case: l2_5_0040
//! doc: stdlib
//! section: 2.5
//! title: A DayOfWeek value supports equality, hashing, casts, array and map keys, and case patterns
//! rule: DateTime.DayOfWeek is a property of type DayOfWeek, an enum over byte with Sunday = 0 through Saturday = 6 (stdlib 2.5); 2024-03-15 is a Friday, value 5. Enum values compare with = and <>, convert explicitly to their underlying integers (spec 5.4) and match case constants.
//! cites: 2.5, 5.4
//! expect: run
//! output: Friday
//!   True
//!   False
//!   True
//!   True
//!   5
//!   5
//!   5
//!   True
//!   FriSatSun
//!   2
//!   3
//!   2
//!   True
//!   True
//!   Fri
//!   weekday
//!   weekend
module Conf;

public class Program
  public static method Name(d: DayOfWeek): string
    return (case d of
      DayOfWeek.Sunday => "Sun", DayOfWeek.Monday => "Mon",
      DayOfWeek.Tuesday => "Tue", DayOfWeek.Wednesday => "Wed",
      DayOfWeek.Thursday => "Thu", DayOfWeek.Friday => "Fri",
      DayOfWeek.Saturday => "Sat");
  end Name;
  public static method Main()
    var d := new DateTime(2024, 3, 15);
    var w := d.DayOfWeek;
    Console.WriteLine(nameof(DayOfWeek.Friday));
    Console.WriteLine(w = DayOfWeek.Friday);
    Console.WriteLine(w <> DayOfWeek.Friday);
    Console.WriteLine(w.Equals(DayOfWeek.Friday));
    Console.WriteLine(w.GetHashCode() = DayOfWeek.Friday.GetHashCode());
    Console.WriteLine(int(w));
    Console.WriteLine(byte(w));
    Console.WriteLine(long(w));
    Console.WriteLine(DayOfWeek(5) = w);
    var arr: Array<DayOfWeek> := [d.DayOfWeek, d.AddDays(1).DayOfWeek, d.AddDays(2).DayOfWeek];
    Console.WriteLine(Name(arr[0]) + Name(arr[1]) + Name(arr[2]));
    var dict := new Dictionary<DayOfWeek, int>();
    dict[d.DayOfWeek] := 1;
    dict[d.AddDays(1).DayOfWeek] := 2;
    dict[d.AddDays(7).DayOfWeek] := 3;
    Console.WriteLine(dict.Count);
    Console.WriteLine(dict[DayOfWeek.Friday]);
    Console.WriteLine(dict[DayOfWeek.Saturday]);
    var o: DayOfWeek? := null;
    Console.WriteLine(o = null);
    o := d.DayOfWeek;
    Console.WriteLine(o = DayOfWeek.Friday);
    Console.WriteLine(Name(o!));
    var r := (case d.DayOfWeek of
      DayOfWeek.Saturday | DayOfWeek.Sunday => "weekend",
      _ => "weekday");
    Console.WriteLine(r);
    var r2 := (case d.AddDays(1).DayOfWeek of
      DayOfWeek.Saturday | DayOfWeek.Sunday => "weekend",
      _ => "weekday");
    Console.WriteLine(r2);
  end Main;
end Program;
