//! case: s27_6_0024
//! doc: spec
//! section: 27.6
//! title: a receiver-qualified reference to a user extension on IList<int> is formed and invoked
//! rule: "Receiver-qualified — `v.Scaled` over `extension Vec`, the member resolved on the value exactly as the call
//!   `v.Scaled()` is (§26.5) — the reference is a bound reference ... the resulting function has the method's own
//!   parameters only" (§27.6, v0.340). The extension on `IList<int>` applies to the `IList<int>`-typed `il`, so
//!   `il.Twice` and `il.Tag` form `() -> int` references, and `il.Tag()` is the same call.
//! cites: 26.5
//! expect: run
//! output: 6
//!   99
//!   99
module Conf;

extension IList<int>
  public method Tag(): int return 99; end Tag;
  public method Twice(): int return 2 * this.Count; end Twice;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    var f: () -> int := il.Twice;
    Console.WriteLine(f());
    var g: () -> int := il.Tag;
    Console.WriteLine(g());
    Console.WriteLine(il.Tag());
  end Main;
end Program;
