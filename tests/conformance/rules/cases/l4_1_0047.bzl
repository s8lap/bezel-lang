//! case: l4_1_0047
//! doc: stdlib
//! section: 4.1
//! title: Contains reads true through an IList-typed receiver
//! rule: `ICollection<T>` declares `method Contains(item: T): bool` (stdlib §4.1, v0.340), and
//!   `IList<T>: IReadOnlyList<T>, ICollection<T>` (§4.1) inherits it; §26.5 rule 1 commits instance
//!   resolution to the head's declared member, so the `Array<int>` (§4.4) holding 1, 2, 3 behind an
//!   `IList<int>`-typed receiver answers `true` for `Contains(2)`.
//! cites: 4.1, 4.4, 26.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    Console.WriteLine(il.Contains(2));
  end Main;
end Program;
