//! case: s27_6_0026
//! doc: spec
//! section: 27.6
//! title: an extension on IReadOnlyList<int> is reached by a bare reference through an IList<int> receiver
//! rule: "the compiler searches ... for extension blocks on `x`'s type, base types, or implemented interfaces" (§26.5
//!   rule 2, v0.340). `IList<T>` is declared `IList<T>: IReadOnlyList<T>, ICollection<T>` (stdlib §4.1), so the
//!   reference `il.OnlyRO` of §27.6 resolves to `extension IReadOnlyList<int>`.
//! cites: 26.5, 4.1
//! expect: run
//! stdout: 5
module Conf;

extension IReadOnlyList<int>
  public method OnlyRO(): int return 5; end OnlyRO;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    var g: () -> int := il.OnlyRO;
    Console.WriteLine(g());
  end Main;
end Program;
