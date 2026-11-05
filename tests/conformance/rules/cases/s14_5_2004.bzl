//! case: s14_5_2004
//! doc: spec
//! section: 14.5
//! title: Math.Clamp over a user record named Guid that is not IComparable
//!   is a constraint error
//! rule: The candidate stays in the set, and if resolution selects it the
//!   call is a compile error naming the constraint
//! cites: 2.2, 14.2
//! expect: reject
//! diagnostic: constraint
module Conf;

public record Guid(X: int);

public class Program
  public static method Main()
    var a := new Guid(1);
    var b := new Guid(3);
    var m := Math.Clamp(a, b, b);
  end Main;
end Program;
