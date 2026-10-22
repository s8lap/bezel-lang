//! case: s22_5_0012
//! doc: spec
//! section: 22.5
//! title: owned defaults of a base primary constructor are filled with and without arguments
//! rule: "A default value is permitted only on parameters with no modifier or `owned` (an `owned` default is the owned-allocation form this section already blesses ...) ... the parameter then receives ownership per §22.3/§28.8 and the callee discharges it" (§22.5 *Defaults and modifiers*, v0.340). The base `Base(owned bs := [1, 2, 3], owned c := ["q"])` is reached through `D(): Base` (defaults: 3 * 10 + 1 = 31), `D2(k): Base([7])` (one supplied, one default: 10 + 1 = 11) and an explicit `base()` call (31).
//! cites: 22.10
//! expect: run
//! stdout: 31 11 31
module Conf;

public class Base(owned bs: Array<int> := [1, 2, 3], owned c: Array<string> := ["q"])
  public method BL(): int => bs.Count * 10 + c.Count;
end Base;

public class D(): Base
end D;

public class D2(k: int): Base([7])
end D2;

public class E: Base
  public constructor() base(); end;
end E;

public class Program
  public static method Main()
    Console.WriteLine($"{new D().BL()} {new D2(1).BL()} {new E().BL()}");
  end Main;
end Program;
