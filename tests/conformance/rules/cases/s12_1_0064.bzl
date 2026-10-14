//! case: s12_1_0064
//! doc: spec
//! section: 12.1
//! title: a swapped generic-base override dispatches to the matching override
//! rule: Visibility, return type, parameter types, and parameter modifiers
//!   must match the base member exactly (v0.338, §12.1). `D<X, Y>` derives
//!   `B<Y, X>`, so the base member it overrides takes `(e, x: Y, y: X)` for
//!   `D`'s own `X` and `Y`, and `override method M<U>(e: U, a: Y, b: X)`
//!   matches it; each base clause instantiates its generic base at its own
//!   arguments (§14.1).
//! cites: 12.1, 14.1
//! expect: run
//! output: 3
//!   2
//!   20
module Conf;

public class B<X, Y>
  public constructor()
  end constructor;

  public virtual method M<U>(e: U, x: X, y: Y): int => 1;
end B;

public class D<X, Y>: B<Y, X>
  public constructor()
  end constructor;

  public override method M<U>(e: U, a: Y, b: X): int => 2;
end D;

public class E: D<int, string>
  public constructor()
  end constructor;

  public override method M<W>(e: W, a: string, b: int): int => 3;
end E;

public class F: D<string, int>
  public constructor()
  end constructor;

  public override method M<W>(e: W, a: int, b: string): int => 20;
end F;

public class Program
  public static method Main(): int
    var b: B<string, int> := new E();
    var c: B<string, int> := new D<int, string>();
    var f: B<int, string> := new F();
    Console.WriteLine(b.M<bool>(true, "s", 1));
    Console.WriteLine(c.M<bool>(true, "s", 1));
    Console.WriteLine(f.M<bool>(true, 1, "s"));
    return 0;
  end Main;
end Program;
