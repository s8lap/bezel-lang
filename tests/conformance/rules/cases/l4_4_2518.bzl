//! case: l4_4_2518
//! doc: stdlib
//! section: 4.4
//! title: Sort() and Sorted() on Array<T> inside a method constrained where T: IComparable<T>
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 4.4, 2.8, 14.2
//! expect: run
//! output: 8:10
//!   9:20
//!   7:30
//!   8:10
//!   9:20
//!   7:30
//!   1
//!   2
//!   3
//! issue: 8584
module Conf;

public record Pair(tag: int, cents: int): IComparable<Pair>
  public method CompareTo(other: Pair): int
    if cents < other.cents then return -1; end if;
    if cents > other.cents then return 1; end if;
    return 0;
  end CompareTo;
end Pair;

public static class G
  public static method SortIt<T>(a: Array<T>) where T: IComparable<T>
    a.Sort();
  end SortIt;

  public static method SortedIt<T>(a: Array<T>): owned Array<T> where T: IComparable<T>
    return a.Sorted();
  end SortedIt;
end G;

public class Program
  public static method Main()
    using var a: Array<Pair> := [new Pair(9, 20), new Pair(7, 30), new Pair(8, 10)];
    G.SortIt<Pair>(a);
    using var s := G.SortedIt<Pair>(a);
    for var e in a do
      Console.WriteLine($"{e.tag}:{e.cents}");
    end;
    for var e in s do
      Console.WriteLine($"{e.tag}:{e.cents}");
    end;
    using var b: Array<int> := [3, 1, 2];
    G.SortIt<int>(b);
    for var e in b do
      Console.WriteLine(e);
    end;
  end Main;
end Program;
