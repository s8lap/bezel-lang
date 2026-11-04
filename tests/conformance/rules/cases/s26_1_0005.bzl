//! case: s26_1_0005
//! doc: spec
//! section: 26.1
//! title: `this` in an extension over TimeOnly and DateOnly reaches built-in TimeOnly and DateOnly parameters
//! rule: `this` refers to the receiver.
//! cites: 26.1
//! expect: run
//! issue: 7304
//! output: 10
//!   True

module Conf;

extension TimeOnly
  public method On(d: DateOnly): DateTime => d.ToDateTime(this);
end;

extension DateOnly
  public method Cmp(o: DateOnly): int => o.CompareTo(this);
end;

public class Program
  public static method Main()
    var t := new TimeOnly(10, 30);
    var d := new DateOnly(2020, 5, 6);
    Console.WriteLine(t.On(d).Hour);
    Console.WriteLine(new DateOnly(2020, 5, 7).Cmp(d) < 0);
  end Main;
end Program;
