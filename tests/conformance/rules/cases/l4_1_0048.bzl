//! case: l4_1_0048
//! doc: stdlib
//! section: 4.1
//! title: Count reads the element count through an IList-typed receiver
//! rule: `IReadOnlyCollection<out T>: IEnumerable<T>` declares `pure readonly property Count: int`
//!   (stdlib §4.1, v0.340), and `IList<T>: IReadOnlyList<T>, ICollection<T>` inherits it; §26.5 rule 1
//!   commits instance resolution to the head's declared member, so the `Array<int>` (§4.4) holding
//!   1, 2, 3 behind an `IList<int>`-typed receiver answers `3`.
//! cites: 4.1, 4.4, 26.5
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    Console.WriteLine(il.Count);
  end Main;
end Program;
