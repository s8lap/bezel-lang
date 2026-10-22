//! case: s22_10_0023
//! doc: spec
//! section: 22.10
//! title: an owned OwningArray primary-constructor parameter given a literal keeps its elements
//! rule: "a primary-constructor parameter may be declared `owned`, and the same ownership-transfer rules as regular constructor
//!   parameters apply" (§22.10, v0.340): `owned xs: OwningArray<R>` given the literal `[new R("a"), new R("b")]` makes the object the
//!   owner of both elements, so `Live()` reads 2 after the literal expression ends, even with another array allocated meanwhile.
//! cites: 28.8
//! expect: run
//! stdout: 2 4 a
module Conf;

public class R(name: string)
  public method N(): string => name;
end R;

public class O(owned xs: OwningArray<R>)
  public method Live(): int => xs.Count;
  public method First(): string => xs[0].N();
end O;

public class Program
  public static method Main()
    var o := new O([new R("a"), new R("b")]);
    var j: Array<int> := [9, 9, 9, 9];
    Console.WriteLine($"{o.Live()} {j.Count} {o.First()}");
  end Main;
end Program;
