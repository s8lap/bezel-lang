//! case: s11_6_0021
//! doc: spec
//! section: 11.6
//! title: a local declared without a type annotation holds a nested type's value from an instance method
//! rule: Elsewhere a chain head reaches its static members, and a local declared without a type
//!   annotation holds a value of it (`var n := tree.Root();`) (§11.6, v0.340) — the value's type
//!   is the nested type, and the local never names it.
//! cites: 11.6
//! expect: run
//! stdout: 7
module Conf;

public class C<T>
  public class N
    public method V(): int => 7;
  end N;

  public method Root(): owned N => new N();
end C;

public class Program
  public static method Main(): int
    using var c := new C<int>();
    using var n := c.Root();
    Console.WriteLine(n.V());
    return 0;
  end Main;
end Program;
