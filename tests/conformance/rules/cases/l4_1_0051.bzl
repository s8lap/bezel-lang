//! case: l4_1_0051
//! doc: stdlib
//! section: 4.1
//! title: IndexOf is not a member of IList and the call is a compile error
//! rule: `public interface IList<T>: IReadOnlyList<T>, ICollection<T>` declares
//!   `property [i: int]: T` and `method Insert(index: int, item: T)` (stdlib §4.1,
//!   v0.340) over the members it inherits — `IndexOf` is declared on the concrete
//!   `Array<T>` (§4.4) alone, and `IList<T>` does not declare it; no stdlib §5 extension
//!   operator is named `IndexOf`, so §26.5 rule 1 finds no instance member and rule 2
//!   finds no extension: the call is a compile error.
//! cites: 4.1, 4.4, 26.5
//! expect: reject
//! diagnostic: no member `IndexOf` on `IList`
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    Console.WriteLine(il.IndexOf(2));
  end Main;
end Program;
