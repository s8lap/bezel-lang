//! case: s27_1_1_8707
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value at an owned return slot of a generic call is admitted
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: run
//! stdout: 1
//! notes: Twin of s27_1_1_8706: the parameter is `() -> owned T`, so `Use<K>` instantiates the
//!   owned slot and the owned-return argument matches it exactly. `Use` disposes the fresh
//!   value it receives from each call of `f()`; it returns 1.
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Use<T>(f: () -> owned T): int
    using var x := f();
    return 1;
  end Use;
  public static method Main(): int
    var f: () -> owned K := lambda() => new K(5);
    Console.WriteLine($"{Program.Use<K>(f)}");
    return 0;
  end Main;
end Program;
