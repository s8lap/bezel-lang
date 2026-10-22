//! case: s22_5_0010
//! doc: spec
//! section: 22.5
//! title: an owned default that mentions an earlier parameter sees that parameter's value
//! rule: "A default value is permitted only on parameters with no modifier or `owned` (an `owned` default is the owned-allocation form this section already blesses ...) ... the parameter then receives ownership per §22.3/§28.8 and the callee discharges it" (§22.5 *Defaults and modifiers*, v0.340). The default `[x, x]` of `owned ys` is evaluated at the construction with `x` bound: `new A(5)` gives two 5s (`2 * 100 + 5 + 5` = 210), and a supplied `[1, 2, 3]` replaces it (`300 + 1 + 2` = 303).
//! cites: 22.10
//! expect: run
//! stdout: 210 303
module Conf;

public class A(x: int, owned ys: Array<int> := [x, x])
  public method S(): int => ys.Count * 100 + ys[0] + ys[1];
end A;

public class Program
  public static method Main()
    Console.WriteLine($"{new A(5).S()} {new A(5, [1, 2, 3]).S()}");
  end Main;
end Program;
