//! case: s14_2_2409
//! doc: spec
//! section: 14.2
//! title: Min()/Max() over two implementing classes of an IComparable-reaching
//!   interface
//! rule: either I is named directly in T's inheritance clause, or T extends an
//!   interface that itself conforms to I
//! cites: 14.2, 26.5
//! expect: run
//! output: 6
//!   64
//! issue: 8186
//! notes: Red on base: the saved pre-fix compiler of the #8186/#8199 review refused
//!   this program with the #2770 'does not - its CompareTo could not be
//!   resolved for dispatch' error, because its no-selector guard read only
//!   direct bases. It passes on the staging tip. Areas: Tri(5)=15, Sq(3)=9,
//!   Tri(2)=6, Sq(8)=64, so Min is 6 and Max is 64 whichever class holds them.
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

  public method Area(): int => _s * _s;

  public method CompareTo(other: IShape): int => Area() - other.Area();
end Sq;

public class Tri: IShape
  private _s: int;

  public constructor(s: int)
    _s := s;
  end constructor;

  public method Area(): int => _s * 3;

  public method CompareTo(other: IShape): int => Area() - other.Area();
end Tri;

public class Program
  public static method Main(): int
    using var a := new Tri(5);
    using var b := new Sq(3);
    using var c := new Tri(2);
    using var d := new Sq(8);
    var arr: Array<IShape> := [a, b, c, d];
    Console.WriteLine(arr.Min().Area());
    Console.WriteLine(arr.Max().Area());
    return 0;
  end Main;
end Program;
