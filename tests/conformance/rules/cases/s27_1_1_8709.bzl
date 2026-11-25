//! case: s27_1_1_8709
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value flowing through a conditional into an owned return slot is
//!   admitted
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: run
//! stdout: 5
//! notes: Twin of s27_1_1_8708: the target slot is `owned`, so both arms match exactly; the owned
//!   result of `h()` is bound with `using var` and disposed once.
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Main(): int
    var f: () -> owned K := lambda() => new K(5);
    var h: () -> owned K := (if true then f else f);
    using var k := h();
    Console.WriteLine($"{k.N}");
    return 0;
  end Main;
end Program;
