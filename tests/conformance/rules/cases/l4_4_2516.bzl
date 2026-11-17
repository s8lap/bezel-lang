//! case: l4_4_2516
//! doc: stdlib
//! section: 4.4
//! title: Sort() orders a record that declares CompareTo and lists IComparable<T>, nested record field read
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 4.4, 2.8, 14.2
//! expect: run
//! output: 2:10
//!   3:20
//!   1:30
//! issue: 8584
module Conf;

public record Inner(x: int, y: int);

public record Outer(tag: int, inner: Inner, z: int): IComparable<Outer>
  public method CompareTo(other: Outer): int
    if inner.y < other.inner.y then return -1; end if;
    if inner.y > other.inner.y then return 1; end if;
    return 0;
  end CompareTo;
end Outer;

public class Program
  public static method Main()
    using var a: Array<Outer> := [new Outer(1, new Inner(1, 30), 5), new Outer(2, new Inner(2, 10), 4), new Outer(3, new Inner(3, 20), 3)];
    a.Sort();
    for var e in a do
      Console.WriteLine($"{e.tag}:{e.inner.y}");
    end;
  end Main;
end Program;
