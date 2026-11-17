//! case: s8_3_2103
//! doc: spec
//! section: 8.3
//! title: GetHashCode through a where T: IEquatable<T> body agrees with the direct hash of an equal Guid
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
    var a := Guid.Parse("00000000-0000-0000-0000-000000000001");
    var b := Guid.Parse("00000000-0000-0000-0000-000000000001");
    var c := Guid.Parse("ffffffff-0000-0000-0000-000000000002");
    Console.WriteLine($"{HE(a) = a.GetHashCode()} {HE(a) = HE(b)} {HE(c) = c.GetHashCode()}");
  end Main;
end Program;
