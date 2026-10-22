//! case: s22_5_0011
//! doc: spec
//! section: 22.5
//! title: an owned generic primary-constructor default builds the literal at the instantiation
//! rule: "A default value is permitted only on parameters with no modifier or `owned` (an `owned` default is the owned-allocation form this section already blesses ...) ... the parameter then receives ownership per §22.3/§28.8 and the callee discharges it" (§22.5 *Defaults and modifiers*, v0.340). For `A<T>(owned xs: Array<T> := [])` the default is the empty literal at `T = int` and `T = string` (0, 0); for `B<T>(x: T, owned xs := [x, x, x])` it holds three elements (3, 3).
//! cites: 22.10, 14.1
//! expect: run
//! stdout: 0 0 3 3
module Conf;

public class A<T>(owned xs: Array<T> := [])
  public method N(): int => xs.Count;
end A;

public class B<T>(x: T, owned xs: Array<T> := [x, x, x])
  public method N(): int => xs.Count;
end B;

public class Program
  public static method Main()
    var a := new A<int>();
    var b := new A<string>();
    var c := new B<int>(5);
    var d := new B<string>("s");
    Console.WriteLine($"{a.N()} {b.N()} {c.N()} {d.N()}");
  end Main;
end Program;
