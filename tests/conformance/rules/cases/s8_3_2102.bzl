//! case: s8_3_2102
//! doc: spec
//! section: 8.3
//! title: GetHashCode through a where T: IEquatable<T> body agrees with the direct hash of an equal TimeSpan
//! rule: `GetHashCode` must satisfy: if `a.Equals(b)` then `a.GetHashCode() = b.GetHashCode()`.
//! cites: 8.3, 14.2
//! expect: run
//! stdout: True True True
//! issue: 8578
module Conf;

public class Program
  public static method HE<T>(x: T): int where T: IEquatable<T>
    return x.GetHashCode();
  end HE;

  public static method Main()
    var a := TimeSpan.FromTicks(5L);
    var b := TimeSpan.FromTicks(5L);
    var c := TimeSpan.FromTicks(9L);
    Console.WriteLine($"{HE(a) = a.GetHashCode()} {HE(a) = HE(b)} {HE(c) = c.GetHashCode()}");
  end Main;
end Program;
