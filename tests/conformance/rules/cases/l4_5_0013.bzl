//! case: l4_5_0013
//! doc: stdlib
//! section: 4.5
//! title: ToArray() is reachable on OwningArray<T> as a borrowing snapshot extension
//! rule: What is reachable, and is not an owning copy, is the LINQ ToArray()
//!   extension on IEnumerable<T>: it resolves on an OwningArray<T> receiver
//!   and returns an owned Array<T> — a borrowing snapshot whose slots hold
//!   borrows of the elements this array still owns.
//! cites: 4.5, 5
//! expect: run
//! stdout: 1
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    using var snapshot := pool.ToArray();
    Console.WriteLine(snapshot.Count);
  end Main;
end Program;
