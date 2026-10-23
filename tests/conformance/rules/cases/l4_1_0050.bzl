//! case: l4_1_0050
//! doc: stdlib
//! section: 4.1
//! title: Insert is not a member of ICollection and the call is a compile error
//! rule: `public interface ICollection<T>: IReadOnlyCollection<T>` declares
//!   `readonly property IsReadOnly: bool`, `method Add(item: T)`, `method Clear()`,
//!   `method Contains(item: T): bool` and `method Remove(item: T): bool` (stdlib §4.1,
//!   v0.340) — `Insert` is declared on `IList<T>` (§4.1) alone, and `ICollection<T>`
//!   does not extend it; no stdlib §5 extension operator is named `Insert`, so §26.5
//!   rule 1 finds no instance member and rule 2 finds no extension: the call is a
//!   compile error.
//! cites: 4.1, 26.5
//! expect: reject
//! diagnostic: no member `Insert` on `ICollection`
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var co: ICollection<int> := a;
    co.Insert(0, 1);
  end Main;
end Program;
