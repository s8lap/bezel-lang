//! case: s26_1_0004
//! doc: spec
//! section: 26.1
//! title: `this` in an extension over TimeSpan reaches DateTime.Add and TimeSpan.CompareTo as the receiver
//! rule: `this` refers to the receiver.
//! cites: 26.1
//! expect: run
//! issue: 7304
//! output: 3
//!   True
//!   False

module Conf;

extension TimeSpan
  public method After(d: DateTime): DateTime => d.Add(this);
  public method Cmp(o: TimeSpan): int => o.CompareTo(this);
end;

public class Program
  public static method Main()
    var start := new DateTime(2020, 1, 1);
    var two := TimeSpan.FromDays(2.0);
    var one := TimeSpan.FromDays(1.0);
    Console.WriteLine(two.After(start).Day);
    Console.WriteLine(one.Cmp(two) > 0);
    Console.WriteLine(two.Cmp(one) > 0);
  end Main;
end Program;
