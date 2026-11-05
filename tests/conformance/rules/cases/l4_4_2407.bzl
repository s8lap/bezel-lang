//! case: l4_4_2407
//! doc: stdlib
//! section: 4.4
//! title: Array<IShape>.Sort() is admitted for an interface reaching IComparable<T>
//!   through IOrd<T>
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2
//! expect: run
//! output: 3
//!   4
//!   5
//!   7
//!   9
//! notes: Red on base: the pre-fix compiler rejected Sort() with 'requires T:
//!   IComparable<T>; IShape does not implement IComparable<IShape>'. Ascending
//!   by CompareTo is Area order 3, 4, 5, 7, 9. It passes on the staging tip. No
//!   open issue describes the Sort() call site (#8186 covers the no-selector
//!   Min()/Max() guard only).
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
    using var s3 := new Sq(5);
    using var s4 := new Sq(9);
    using var s5 := new Sq(4);
    var arr: Array<IShape> := [s1, s2, s3, s4, s5];
    arr.Sort();
    for var e in arr do
      Console.WriteLine(e.Area());
    end;
    return 0;
  end Main;
end Program;
