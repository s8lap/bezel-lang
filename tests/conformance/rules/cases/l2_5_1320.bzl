//! case: l2_5_1320
//! doc: stdlib
//! section: 2.5
//! title: DateTime Add*, Add, Subtract(span) and the +/- operators with a TimeSpan keep the operand's Kind
//! rule: keep the `DateTime` operand's `Kind`
//! cites: 2.5
//! expect: run
//! output: 1 1 1 1 1 1 1 1
//!   2 2 2 2 2
//!   1 1 1
module Conf;

public class Program
  static method K(k: DateTimeKind): int
    if k = DateTimeKind.Utc then
      return 1;
    end;
    if k = DateTimeKind.Local then
      return 2;
    end;
    return 0;
  end K;

  public static method Main()
    var d := new DateTime(2024, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc);
    var l := new DateTime(2024, 1, 1, 0, 0, 0, 0, DateTimeKind.Local);
    var t := TimeSpan.FromTicks(5);
    Console.WriteLine($"{K(d.AddDays(1.0).Kind)} {K(d.AddYears(1).Kind)} {K(d.AddMonths(1).Kind)} {K((d + t).Kind)} {K(d.Subtract(t).Kind)} {K((t + d).Kind)} {K((d - t).Kind)} {K(d.Add(t).Kind)}");
    Console.WriteLine($"{K(l.AddYears(-1).Kind)} {K(l.AddMonths(-3).Kind)} {K(l.AddHours(2.5).Kind)} {K((l - t).Kind)} {K((t + l).Kind)}");
    var u := new DateTime(9999, 12, 31, 0, 0, 0, 0, DateTimeKind.Utc);
    Console.WriteLine($"{K(u.AddMonths(0).Kind)} {K(u.AddYears(0).Kind)} {K(u.AddDays(0.5).Kind)}");
  end Main;
end Program;
