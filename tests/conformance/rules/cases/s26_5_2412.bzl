//! case: s26_5_2412
//! doc: spec
//! section: 26.5
//! title: For an interface element reaching IComparable<T> the stdlib Min()/Max()
//!   beats extension Object
//! rule: An extension on E1 is **more specific** than one on E2 when E1 is a
//!   subtype of E2 and E2 is not a subtype of E1.
//! cites: 26.5, 14.2
//! expect: run
//! output: 3
//!   7
//! issue: 8186
//! notes: Red on base: the saved pre-fix compiler refused the Min()/Max() calls with
//!   the #2770 error (IShape reaches IComparable<IShape> only through
//!   IOrd<IShape>), so the call never resolved to either candidate. Once IShape
//!   conforms (spec 14.2 rule 3) both candidates apply, and the stdlib one is
//!   more specific than the extension on Object. It passes on the staging tip.
module Conf;

public interface IOrd<T>: IComparable<T>
end IOrd;

public interface IShape: IOrd<IShape>
  method Area(): int;
end IShape;

extension Object
  public method Min(): int => 4242;
  public method Max(): int => 4343;
end;

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
    using var s3 := new Sq(5);
    var arr: Array<IShape> := [s1, s2, s3];
    Console.WriteLine(arr.Min().Area());
    Console.WriteLine(arr.Max().Area());
    return 0;
  end Main;
end Program;
