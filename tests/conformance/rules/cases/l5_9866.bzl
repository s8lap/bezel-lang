//! case: l5_9866
//! doc: stdlib
//! section: 5
//! title: Max over a generic body where the selector's U differs from T returns U unnarrowed
//! rule: public method Max<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 5, 3.6, 14.2
//! expect: run
//! output: 9
//!   4.5
//!   9000000000
//! notes: Harvested from the #8689 review (d_Max_TU, without its string-selector line, which the
//!   review found to be an open leak shape). A generic helper `H<T, U>(a, f)` forwards its selector
//!   parameter to Max, so the result type is the caller's U (3.6 rule 2, the substitution at the
//!   call), here int, double and long in turn from the one source of Box values 3, 9, 5:
//!   the largest key per instantiation: V = 9, 9 / 2.0 = 4.5, 9 * 1000000000 = 9000000000.
module Conf;

public class Box
  private _v: int;
  public constructor(v: int)
    _v := v;
  end constructor;
  public method V(): int => _v;
end Box;

public class Program
  static method H<T, U>(a: Array<T>, f: (T) -> U): U where U: IComparable<U> => a.Max(f);

  public static method Main()
    var a := new Array<Box>();
    using var b1 := new Box(3);
    using var b2 := new Box(9);
    using var b3 := new Box(5);
    a.Add(b1);
    a.Add(b2);
    a.Add(b3);
    Console.WriteLine(H(a, lambda(b) => b.V()));
    Console.WriteLine(H(a, lambda(b) => double(b.V()) / 2.0));
    Console.WriteLine(H(a, lambda(b) => long(b.V()) * 1000000000L));
  end Main;
end Program;
