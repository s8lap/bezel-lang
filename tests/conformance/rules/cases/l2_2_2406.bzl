//! case: l2_2_2406
//! doc: stdlib
//! section: 2.2
//! title: Math.Min and Math.Max accept an interface reaching IComparable<T> through
//!   its extension chain
//! rule: public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
//! cites: 14.2
//! expect: run
//! output: 3
//!   7
//! notes: Red on base: the pre-fix compiler rejected Math.Min/Math.Max at IShape
//!   with 'does not satisfy the constraint where T: IComparable<T>', reading
//!   only direct bases. Spec 14.2 rule 3 makes IShape conform through
//!   IOrd<IShape>. It passes on the staging tip. No open issue describes the
//!   Math.Min/Math.Max call site (#8186 covers the no-selector Min()/Max()
//!   guard only).
module Conf;

public interface IOrd<T>: IComparable<T>
end IOrd;

public interface IShape: IOrd<IShape>
  method Area(): int;
end IShape;

public class Sq: IShape
  private _s: int;

  public constructor(s: int)
    _s := s;
  end constructor;

  public method Area(): int => _s;

  public method CompareTo(other: IShape): int => Area() - other.Area();
end Sq;

public class Program
  public static method Main(): int
    using var s1 := new Sq(7);
    using var s2 := new Sq(3);
    var x: IShape := s1;
    var y: IShape := s2;
    Console.WriteLine(Math.Min(x, y).Area());
    Console.WriteLine(Math.Max(x, y).Area());
    return 0;
  end Main;
end Program;
