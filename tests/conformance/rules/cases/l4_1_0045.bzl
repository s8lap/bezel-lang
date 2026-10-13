//! case: l4_1_0045
//! doc: stdlib
//! section: 4.1
//! title: an ICollection's Count reads the number of elements
//! rule: `public interface IReadOnlyCollection<out T>: IEnumerable<T>` declares `pure readonly property Count: int;` (stdlib §4.1), and ICollection<T> extends it.
//!   After two Add calls an Array<int> reached through ICollection<int> has Count 2.
//! cites: 4.1, 4.4
//! expect: run
//! stdout: 2
//! issue: 4704
module Conf;

public class C
  public static method Main(): int
    var c: ICollection<int> := new Array<int>();
    c.Add(1);
    c.Add(2);
    Console.WriteLine(c.Count);
    return 0;
  end Main;
end C;
