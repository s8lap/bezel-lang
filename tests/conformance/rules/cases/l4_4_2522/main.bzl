//! case: l4_4_2522
//! doc: stdlib
//! section: 4.4
//! title: records of one name in two modules each sort by their own CompareTo, generic record included
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 4.4, 2.8, 14.2
//! expect: run
//! output: 8:10
//!   9:20
//!   7:30
//!   7:30
//!   9:20
//!   8:10
//!   2:10
//!   3:20
//!   1:30
//! issue: 8584
module Conf;

import Lib;

public record Pair(tag: int, cents: int): IComparable<Pair>
  public method CompareTo(other: Pair): int
    if cents < other.cents then return 1; end if;
    if cents > other.cents then return -1; end if;
    return 0;
  end CompareTo;
end Pair;

public class Program
  public static method Main()
    using var a: Array<Lib.Pair> := [new Lib.Pair(9, 20), new Lib.Pair(7, 30), new Lib.Pair(8, 10)];
    using var b: Array<Pair> := [new Pair(9, 20), new Pair(7, 30), new Pair(8, 10)];
    using var g: Array<Lib.G<int>> := [new Lib.G<int>(1, 30, 7), new Lib.G<int>(2, 10, 8), new Lib.G<int>(3, 20, 9)];
    a.Sort();
    b.Sort();
    g.Sort();
    for var e in a do
      Console.WriteLine($"{e.tag}:{e.cents}");
    end;
    for var e in b do
      Console.WriteLine($"{e.tag}:{e.cents}");
    end;
    for var e in g do
      Console.WriteLine($"{e.k}:{e.n}");
    end;
  end Main;
end Program;
