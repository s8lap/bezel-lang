//! case: s27_1_1_8837
//! doc: spec
//! section: 27.1.1
//! title: a plain `() -> K` argument is rejected at a `() -> owned K` method parameter
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: reject
//! diagnostic: modifier slots
//! notes: Pins #9127 (fix 25b50939d). The mismatch at a call argument against a non-generic parameter. Accepting twin: s28_3_5_8843 (the same shapes at an `owned` source slot).
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
  public static method Take(f: () -> owned K): int
    using var z := f();
    return z.N;
  end Take;
  public static method Pass(f: () -> K): int
    return Program.Take(f);
  end Pass;
  public static method Main(): int
    Console.WriteLine("unused");
    return 0;
  end Main;
end Program;
