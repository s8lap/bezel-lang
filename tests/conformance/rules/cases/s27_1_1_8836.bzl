//! case: s27_1_1_8836
//! doc: spec
//! section: 27.1.1
//! title: a plain `() -> IK` value is rejected at a `() -> owned IK` target (interface slot)
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: reject
//! diagnostic: modifier slots
//! notes: Pins #9127 (fix 25b50939d). The same mismatch at an interface-typed return slot. Accepting twin: s28_3_5_8843 (the same shapes at an `owned` source slot).
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
    using var k := new KI();
    var f: () -> IK := lambda() => k;
    var g: () -> owned IK := f;
    using var z := g();
    Console.WriteLine($"{z.V()}");
    return 0;
  end Main;
end Program;
