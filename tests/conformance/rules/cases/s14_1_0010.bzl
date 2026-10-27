//! case: s14_1_0010
//! doc: spec
//! section: 14.1
//! title: a generic method's instantiation reads the SUBSTITUTED element type
//! rule: §3.6 rule 2 - "Per-instantiation evaluation for generics. For a
//!   generic type or member, the property is evaluated at each instantiation,
//!   after substituting the type arguments." §14.1's instantiation site is
//!   where a generic method's type arguments are substituted, and stdlib §4.4
//!   declares `Array<T>`'s element type to be `T` - so at the instantiation
//!   `F<Array<int>>` the index expression `x[0]` reads an `int` element, the
//!   substituted element type, and yields the array's first element. §23.4's
//!   indexer is the member the expression selects.
//! cites: 14.1, 3.6, 4.4
//! expect: run
//! stdout: 1
//! issue: 7203
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    Console.WriteLine(F<Array<int>>(a));
  end Main;

  public static method F<T>(x: T): int
    var v: int := x[0];
    return v;
  end F;
end Program;
