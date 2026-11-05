//! case: s14_2_2404
//! doc: spec
//! section: 14.2
//! title: A swapped type-parameter order in the base list still reaches
//!   IComparable<IShape>
//! rule: either I is named directly in T's inheritance clause, or T extends an
//!   interface that itself conforms to I
//! cites: 14.2, 26.5
//! expect: run
//! output: 3
//!   9
//! issue: 8186
//! notes: Red on base: the saved pre-fix compiler of the #8186/#8199 review refused
//!   this program with the #2770 'does not - its CompareTo could not be
//!   resolved for dispatch' error, because its no-selector guard read only
//!   direct bases. It passes on the staging tip. IOrd<int, IShape> extends
//!   IComparable<IShape> because the second argument fills B; the areas are 7,
//!   3, 5, 9, 4. The rejecting twin is the next case.
module Conf;

public interface IOrd<A, B>: IComparable<B>
end IOrd;

public interface IShape: IOrd<int, IShape>
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
    using var s3 := new Sq(5);
    using var s4 := new Sq(9);
    using var s5 := new Sq(4);
    var arr: Array<IShape> := [s1, s2, s3, s4, s5];
    Console.WriteLine(arr.Min().Area());
    Console.WriteLine(arr.Max().Area());
    return 0;
  end Main;
end Program;
