//! case: l9_7_0001
//! doc: stdlib
//! section: 9.7
//! title: the predicate count on an Array<T> receiver does not reach the same-named operator
//! rule: The LINQ extensions in §5 apply to `Array<T>` because it implements
//!   `IEnumerable<T>` — except the four whose names `Array<T>` itself declares:
//!   `Contains`, `Count`, `Reverse`, and `ToArray` resolve to the instance
//!   members and the same-named §5 operators are unreachable on an `Array<T>`
//!   receiver — `arr.Count(pred)` does not resolve to LINQ's predicate count.
//!   Under spec §26.5 rule 1 the instance member wins by name — of any arity,
//!   applicability, or accessibility — so the committed instance lookup is
//!   `Array<T>.Count`, which takes no argument; the predicate count supplies
//!   one, no candidate survives, and the call is a compile error.
//! cites: 9.7, 26.5, 4.4
//! expect: reject
//! diagnostic: instance member
module Conf;

public class Program
  public static method Main()
    var xs: Array<string> := ["a", "b"];
    var n := xs.Count(lambda(s: string): bool => true);
    Console.WriteLine(n);
  end Main;
end Program;
