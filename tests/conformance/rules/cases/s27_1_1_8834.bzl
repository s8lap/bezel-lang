//! case: s27_1_1_8834
//! doc: spec
//! section: 27.1.1
//! title: a conditional of two plain `() -> K` values is rejected at a `() -> owned K` target
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: reject
//! diagnostic: modifier slots
//! notes: Pins #9127 (fix 25b50939d). A conditional whose arms are plain function values is formed at an `owned` target slot; the slots differ. Accepting twin: s28_3_5_8843 (the same shapes at an `owned` source slot).
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;
public interface IK
  method V(): int;
end IK;
public class KI: IK
  public constructor() end constructor;
  public method V(): int => 9;
end KI;
public class Program
  public static method Main(): int
    using var k := new K(7);
    var c := true;
    var f: () -> K := lambda() => k;
    var f2: () -> K := lambda() => k;
    var g: () -> owned K := (if c then f else f2);
    using var z := g();
    Console.WriteLine($"{z.N}");
    return 0;
  end Main;
end Program;
