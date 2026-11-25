//! case: s28_3_5_8840
//! doc: spec
//! section: 28.3.5
//! title: a conditional of two fresh-owned lambdas is admitted at a `() -> owned K` target
//! rule: every return path supplies a fresh or owned value
//! cites: 27.1.1, 28.3.5
//! expect: run
//! output: 5
//! notes: Pins #9127 (fix 25b50939d). Admitting control (twin of 8834): both arms return a fresh `new K(..)`, so the owned slot is honest. c is true: 5.
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
    var c := true;
    var g: () -> owned K := (if c then lambda() => new K(5) else lambda() => new K(6));
    using var z := g();
    Console.WriteLine($"{z.N}");
    return 0;
  end Main;
end Program;
