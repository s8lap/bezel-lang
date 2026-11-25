//! case: l5_9865
//! doc: stdlib
//! section: 5
//! title: Max with an identity selector at a generic T := a user IComparable class returns the largest
//! rule: public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5, 14.2
//! expect: run
//! stdout: 9
//! notes: Harvested from the #8689 review (g_Max_class). U := T := Box, a class implementing
//!   IComparable<Box> by the difference of its values; the largest of 3, 9, 5 is the Box holding 9.
//!   The result is an element of the array, borrowed from it, and read through `V()`.
module Conf;

public class Box : IComparable<Box>
  private _v: int;
  public constructor(v: int)
    _v := v;
  end constructor;
  public method V(): int => _v;
  public method CompareTo(other: Box): int => _v - other.V();
end Box;

public class Program
  static method H<T>(a: Array<T>): T where T: IComparable<T> => a.Max(lambda(x) => x);

  public static method Main()
    var a := new Array<Box>();
    using var b1 := new Box(3);
    using var b2 := new Box(9);
    using var b3 := new Box(5);
    a.Add(b1);
    a.Add(b2);
    a.Add(b3);
    Console.WriteLine(H(a).V());
  end Main;
end Program;
