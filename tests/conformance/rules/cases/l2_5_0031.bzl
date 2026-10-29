//! case: l2_5_0031
//! doc: stdlib
//! section: 2.5
//! title: The eight-argument DateTime keeps its Kind; DateOnly takes named year, month, day in any order
//! rule: DateTime declares `constructor(..., millisecond := 0, kind := DateTimeKind.Unspecified)`, so the
//!   eighth positional argument is `kind`; named `DateOnly(day := 2, month := 5, year := 2000)` binds each
//!   parameter by name (spec 22.5) and renders `yyyy-MM-dd`.
//! cites: 2.5, 22.5
//! expect: run
//! issue: 7536
//! output: True
//!   2000-05-02
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2024, 3, 4, 5, 6, 7, 8, DateTimeKind.Utc);
    Console.WriteLine(d.Kind = DateTimeKind.Utc);
    var o := new DateOnly(day := 2, month := 5, year := 2000);
    Console.WriteLine(o);
  end Main;
end Program;
