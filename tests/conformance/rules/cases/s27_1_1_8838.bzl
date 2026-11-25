//! case: s27_1_1_8838
//! doc: spec
//! section: 27.1.1
//! title: a plain nullable `(() -> K)?` value is rejected at a `(() -> owned K)?` target
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: reject
//! diagnostic: modifier slots
//! notes: Pins #9127 (fix 25b50939d). The mismatch under a nullable function type: nullability does not relate the modifier slots. Accepting twin: s28_3_5_8843 (the same shapes at an `owned` source slot).
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
    var f: (() -> K)? := lambda() => k;
    var g: (() -> owned K)? := f;
    Console.WriteLine("unused");
    return 0;
  end Main;
end Program;
