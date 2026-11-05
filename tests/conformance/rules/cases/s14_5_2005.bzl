//! case: s14_5_2005
//! doc: spec
//! section: 14.5
//! title: Math.Clamp over a user record named Guid that implements
//!   IComparable orders by the user CompareTo
//! rule: The candidate stays in the set, and if resolution selects it the
//!   call is a compile error naming the constraint
//! cites: 2.2, 14.2
//! expect: run
//! stdout: 3
module Conf;

public record Guid(X: int): IComparable<Guid>
  public method CompareTo(other: Guid): int => X - other.X;
end Guid;

public class Program
  public static method Main()
    var a := new Guid(1);
    var b := new Guid(3);
    var m := Math.Clamp(a, b, b);
    Console.WriteLine(m.X);
  end Main;
end Program;
