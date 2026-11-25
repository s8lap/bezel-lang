//! case: s28_3_5_8842
//! doc: spec
//! section: 28.3.5
//! title: a lambda whose body is a conditional of fresh values is admitted at a `() -> owned K` target
//! rule: every return path supplies a fresh or owned value
//! cites: 27.1.1, 28.3.5
//! expect: run
//! output: 1
//! notes: Pins #9127 (fix 25b50939d). Admitting control: every return path of the lambda body (both conditional arms) is a fresh `new K(..)`. c is true: 1.
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
    var g: () -> owned K := lambda() => (if c then new K(1) else new K(2));
    using var z := g();
    Console.WriteLine($"{z.N}");
    return 0;
  end Main;
end Program;
