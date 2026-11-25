//! case: s28_3_5_8841
//! doc: spec
//! section: 28.3.5
//! title: a method group of an `owned K` method is admitted at a `() -> owned K` target
//! rule: every return path supplies a fresh or owned value
//! cites: 27.1.1, 28.3.5
//! expect: run
//! output: 5
//! notes: Pins #9127 (fix 25b50939d). Admitting control: `Program.Fresh` returns `owned K`; the method reference's slot is `owned`, so it matches. Result 5.
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
  public static method Fresh(): owned K => new K(5);
  public static method Main(): int
    var g: () -> owned K := Program.Fresh;
    using var z := g();
    Console.WriteLine($"{z.N}");
    return 0;
  end Main;
end Program;
