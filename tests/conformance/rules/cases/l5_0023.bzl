//! case: l5_0023
//! doc: stdlib
//! section: 5
//! title: Contains reaches the §5 operator on an IReadOnlyList-typed receiver
//! rule: §5 declares `public extension IEnumerable<T> where T: hashable` with
//!   `public method Contains(value: T): bool;` (stdlib §5, v0.340). `IReadOnlyList<T>`
//!   declares no instance member named `Contains` — `Contains` is declared on
//!   `ICollection<T>` (stdlib §4.1), which `IReadOnlyList<T>` does not extend — so §26.5
//!   rule 1 does not commit instance resolution and rule 2 reaches the §5 extension
//!   block over `IEnumerable<T>`, whose block parameter is inferred as `int` and whose
//!   `where T: hashable` constraint is satisfied. `Contains(3)` over the elements 1, 2, 3
//!   is therefore `true`.
//! cites: 5, 4.1, 26.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var ro: IReadOnlyList<int> := a;
    Console.WriteLine(ro.Contains(3));
  end Main;
end Program;
