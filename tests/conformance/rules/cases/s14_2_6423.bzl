//! case: s14_2_6423
//! doc: spec
//! section: 14.2
//! title: a record listing IEquatable<DateTime> satisfies a where T:
//!   IEquatable<DateTime> bound
//! rule: that lists I (or any interface extending I) in its inheritance clause and
//!   declares matching members
//! cites: 14.2
//! expect: run
//! stdout: accepted
module Conf;
public class Program
  public static method Main(): int
    return F(new RecE());
  end Main;
  public static method F<T>(x: T): int where T: IEquatable<DateTime>
    Console.WriteLine("accepted");
    return 0;
  end F;
end Program;
public record RecE: IEquatable<DateTime>
  public method Equals(other: DateTime): bool
    return true;
  end Equals;
end RecE;
