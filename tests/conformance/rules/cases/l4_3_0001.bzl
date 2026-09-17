//! case: l4_3_0001
//! doc: stdlib
//! section: 4.3
//! title: a read-only helper accepts both a borrowing and an owning collection
//! rule: The two hierarchies share IEnumerable<T>, IReadOnlyCollection<T>,
//!   and IReadOnlyList<T> as common ancestors; a function that wants to
//!   read elements without taking ownership accepts one of these
//!   interfaces, and both borrowing and owning collections of T conform.
//! cites: 4.3, 28.11
//! expect: run
//! stdout: 2
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public static class Helpers
  public static method CountThem<T>(items: IReadOnlyCollection<T>) where T: class
    Console.WriteLine(items.Count);
  end CountThem;
end Helpers;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    pool.Add(new Widget());
    Helpers.CountThem(pool);
  end Main;
end Program;
