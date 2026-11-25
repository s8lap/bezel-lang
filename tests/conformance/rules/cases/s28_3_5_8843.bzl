//! case: s28_3_5_8843
//! doc: spec
//! section: 28.3.5
//! title: an `owned`-slot function value is admitted at every `owned`-slot target the reject cases use
//! rule: every return path supplies a fresh or owned value
//! cites: 27.1.1, 28.3.5
//! expect: run
//! output: 5
//!  5
//!  1
//!  9
//! notes: Pins #9127 (fix 25b50939d). Accepting twin of 8834-8839: the SAME targets (a local, a method parameter, a generic parameter, an interface slot),
//!  but the source's return slot is `owned` as well, so only the source slot differs. Output: 5, 5, 1, 9.
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
  public static method Run<T>(f: () -> owned T): int
    using var z := f();
    return 1;
  end Run;
  public static method Main(): int
    var f: () -> owned K := lambda() => new K(5);
    var g: () -> owned K := f;
    using var z := g();
    Console.WriteLine($"{z.N}");
    Console.WriteLine($"{Program.Take(f)}");
    Console.WriteLine($"{Program.Run<K>(f)}");
    var h: () -> owned IK := lambda() => new KI();
    var h2: () -> owned IK := h;
    using var y := h2();
    Console.WriteLine($"{y.V()}");
    return 0;
  end Main;
end Program;
