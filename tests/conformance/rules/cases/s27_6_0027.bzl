//! case: s27_6_0027
//! doc: spec
//! section: 27.6
//! title: an extension on IReadOnlyList<int> is not reached by a bare reference via IEnumerable<int>
//! rule: "the compiler searches ... for extension blocks on `x`'s type, base types, or implemented interfaces" (§26.5
//!   rule 2, v0.340). `IEnumerable<T>` neither is nor extends `IReadOnlyList<T>` (stdlib §4.1, §4.12), so no extension
//!   applies to `e.OnlyRO`, and `IEnumerable<T>` declares no such member: the reference is rejected as `e.OnlyRO()` is.
//! cites: 26.5, 4.1, 4.12
//! expect: reject
//! diagnostic: onlyro
module Conf;

extension IReadOnlyList<int>
  public method OnlyRO(): int return 5; end OnlyRO;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var e: IEnumerable<int> := a;
    var h: () -> int := e.OnlyRO;
    Console.WriteLine(h());
  end Main;
end Program;
