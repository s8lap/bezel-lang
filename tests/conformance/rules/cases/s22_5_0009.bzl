//! case: s22_5_0009
//! doc: spec
//! section: 22.5
//! title: an owned primary-constructor parameter with a literal default owns it for the object lifetime
//! rule: "A default value is permitted only on parameters with no modifier or `owned` (an `owned` default is the owned-allocation form this section already blesses ...) ... the parameter then receives ownership per §22.3/§28.8 and the callee discharges it" (§22.5 *Defaults and modifiers*, v0.340). So `owned xs: Array<T> := [..]` filled from its default is an array the object owns and reads correctly after the constructing expression: ints (3), strings (2 + len("bb") = 4) and records (1 + 1 = 2), and the supplied-argument form (`[9, 9]` -> 2; `["q", "rr"]` -> 4) behaves the same. The plain-parameter twin is not pinned here: a literal given to a stored plain parameter faults when read later (#7045).
//! cites: 22.10, 28.8
//! expect: run
//! stdout: 3 4 2 2 4
module Conf;

public record Product(Name: string, Qty: int);

public class OI(owned xs: Array<int> := [1, 2, 3])
  public method N(): int => xs.Count;
end OI;

public class OS(owned xs: Array<string> := ["a", "bb"])
  public method N(): int => xs.Count + xs[1].Length;
end OS;

public class OR(owned xs: Array<Product> := [new Product("x", 1)])
  public method N(): int => xs.Count + xs[0].Qty;
end OR;

public class Program
  public static method Main()
    var a := new OI();
    var b := new OS();
    var c := new OR();
    var o1 := new OI([9, 9]);
    var o2 := new OS(["q", "rr"]);
    Console.WriteLine($"{a.N()} {b.N()} {c.N()} {o1.N()} {o2.N()}");
  end Main;
end Program;
