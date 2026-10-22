//! case: s27_6_0025
//! doc: spec
//! section: 27.6
//! title: an extension on IEnumerable<int> is reached by a bare reference via three receiver types
//! rule: "the compiler searches ... for extension blocks on `x`'s type, base types, or implemented interfaces" (§26.5
//!   rule 2, v0.340). `IList<T>` and `IReadOnlyList<T>` extend `IEnumerable<T>` (stdlib §4.1), so the receiver-
//!   qualified reference of §27.6 `r.Tag` resolves to `extension IEnumerable<int>` at all three static types.
//! cites: 26.5, 4.1
//! expect: run
//! output: 99
//!   99
//!   99
module Conf;

extension IEnumerable<int>
  public method Tag(): int return 99; end Tag;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    var g: () -> int := il.Tag;
    Console.WriteLine(g());
    var e: IEnumerable<int> := a;
    var h: () -> int := e.Tag;
    Console.WriteLine(h());
    var r: IReadOnlyList<int> := a;
    var k: () -> int := r.Tag;
    Console.WriteLine(k());
  end Main;
end Program;
