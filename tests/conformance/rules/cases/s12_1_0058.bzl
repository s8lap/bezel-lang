//! case: s12_1_0058
//! doc: spec
//! section: 12.1
//! title: an override's own type parameter matches by position, not by name
//! rule: An override declares exactly as many type parameters of its own as
//!   the overridden member (none where that member declares none), the two
//!   lists corresponding by position, not by name (v0.338, §12.1):
//!   `public override method Echo<U>(x: U): string` overrides
//!   `public virtual method Echo<T>(x: T): string`. The base class's `U` is
//!   not in scope in `H`, so the override's `U` is its own.
//! cites: 12.1, 14.1
//! expect: run
//! issue: 6298
//! stdout: 2
module Conf;

public class G<U>
  public constructor()
  end constructor;

  public virtual method Q<T>(e: T, u: U): int => 1;
end G;

public class H: G<int>
  public constructor()
  end constructor;

  public override method Q<U>(e: U, t: int): int => 2;
end H;

public class Program
  public static method Main(): int
    var g: G<int> := new H();
    Console.WriteLine(g.Q<string>("x", 1));
    return 0;
  end Main;
end Program;
