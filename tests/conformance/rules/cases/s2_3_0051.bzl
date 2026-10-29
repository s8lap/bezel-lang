//! case: s2_3_0051
//! doc: spec
//! section: 2.3
//! title: A user record named TimeSpan shadows the System one including its operator and members
//! rule: Language spec §2.3 (v0.350): an auto-imported System name such as TimeSpan "may be shadowed by local declarations under the normal name-resolution rules". The module's own `record TimeSpan(N: long)` with its own Add, Negate and `operator *` is what `a.Add(b)` and `a * 3.0` use: 5+7=12, Negate(5)=-5, and the declared `*` returns N*3 = 15.
//! cites: 2.3, 9.1
//! expect: run
//! stdout: 12 -5 15
module Conf;

public record TimeSpan(N: long)
  public method Add(o: TimeSpan): TimeSpan => new TimeSpan(N + o.N);
  public method Negate(): TimeSpan => new TimeSpan(0L - N);
  public static method operator *(a: TimeSpan, f: double): TimeSpan => new TimeSpan(a.N * 3L);
end TimeSpan;

public class Program
  public static method Main()
    var a := new TimeSpan(5L);
    var b := new TimeSpan(7L);
    var s := a.Add(b);
    var m := a * 3.0;
    Console.WriteLine($"{s.N} {a.Negate().N} {m.N}");
  end Main;
end Program;
