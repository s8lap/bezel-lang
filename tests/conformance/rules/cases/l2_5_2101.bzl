//! case: l2_5_2101
//! doc: stdlib
//! section: 2.5
//! title: a compound DateTime += / -= on a field and an array element keeps
//!   the operand's Kind
//! rule: The `Add*` family, `Add`, `Subtract(span)` and the `+`/`-`
//!   operators with a `TimeSpan` keep the `DateTime` operand's `Kind`
//! cites: 6.6
//! expect: run
//! output: True
//!   True
//! notes: Green on base and on the staging tip: the Kind-carrying half of
//!   the #8245 compound-assignment path.
module Conf;

public class H
  public D: DateTime;

  public constructor(d: DateTime)
    D := d;
  end constructor;
end H;

public class Program
  public static method Main()
    var h := new H(new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc));
    h.D += TimeSpan.FromDays(1.0);
    Console.WriteLine(h.D.Kind = DateTimeKind.Utc);
    var a := new Array<DateTime>(1, new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Local));
    a[0] -= TimeSpan.FromDays(1.0);
    Console.WriteLine(a[0].Kind = DateTimeKind.Local);
  end Main;
end Program;
