//! case: l9_7_0002
//! doc: stdlib
//! section: 9.7
//! title: the no-argument Count on an Array<T> receiver is the instance member
//! rule: The LINQ extensions in §5 apply to `Array<T>` because it implements
//!   `IEnumerable<T>` — except the four whose names `Array<T>` itself declares:
//!   `Contains`, `Count`, `Reverse`, and `ToArray` resolve to the instance
//!   members and the same-named §5 operators are unreachable on an `Array<T>`
//!   receiver. `Array<T>` declares `public pure readonly property Count: int`
//!   (stdlib §4.4), so the no-argument read is that instance member's own form
//!   and is admitted — it is the predicate count, not the instance member, that
//!   matches no declared form.
//! cites: 9.7, 26.5, 4.4
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    var xs: Array<string> := ["a", "b"];
    Console.WriteLine(xs.Count);
  end Main;
end Program;
