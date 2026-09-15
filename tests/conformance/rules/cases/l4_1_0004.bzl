//! case: l4_1_0004
//! doc: stdlib
//! section: 4.1
//! title: IList<T> is a reachable abstraction over Array<T>'s mutation surface
//! rule: IList<T>: IReadOnlyList<T>, ICollection<T> declares the settable
//!   indexer and Insert(index, item); Array<T> conforms to IList<T>, so a
//!   generic helper written against the interface reaches those members.
//! cites: 4.1, 4.4
//! expect: run
//! stdout: 1
module Conf;

public method InsertFirst<T>(items: IList<T>, value: T)
  items.Insert(0, value);
end InsertFirst;

public class Program
  public static method Main()
    using var arr: Array<int> := [2, 3];
    InsertFirst(arr, 1);
    Console.WriteLine(arr[0]);
  end Main;
end Program;
