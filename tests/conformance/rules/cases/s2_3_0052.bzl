//! case: s2_3_0052
//! doc: spec
//! section: 2.3
//! title: A user class named TimeSpan shadows the System one including Add and Subtract
//! rule: Language spec §2.3 (v0.350): an auto-imported System name such as TimeSpan "may be shadowed by local declarations under the normal name-resolution rules". The module's own `class TimeSpan` with Add, Subtract and Negate is used for `a.Add(b)` etc.: 12, 5-7=-2, -5.
//! cites: 2.3, 8.1
//! expect: run
//! stdout: 12 -2 -5
module Conf;

public class TimeSpan
  public T: long := 0L;
  public constructor(t: long)
    this.T := t;
  end constructor;
  public method Add(o: TimeSpan): owned TimeSpan => new TimeSpan(this.T + o.T);
  public method Subtract(o: TimeSpan): owned TimeSpan => new TimeSpan(this.T - o.T);
  public method Negate(): owned TimeSpan => new TimeSpan(0L - this.T);
end TimeSpan;

public class Program
  public static method Main()
    var a := new TimeSpan(5L);
    var b := new TimeSpan(7L);
    Console.WriteLine($"{a.Add(b).T} {a.Subtract(b).T} {a.Negate().T}");
  end Main;
end Program;
