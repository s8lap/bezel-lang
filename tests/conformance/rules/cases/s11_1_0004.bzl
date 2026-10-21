//! case: s11_1_0004
//! doc: spec
//! section: 11.1
//! title: a two-instantiation class answers a generic-body interface call through its instantiation
//! rule: "A generic interface may be listed at **two or more instantiations** (`class D: IComparable<int>, IComparable<string>`), directly or through a base class. The class still declares **one** member set, and [§11.2]'s matching rule is applied once per obligation, each under its own instantiation's substitution" ([§11.1]); the call `i.Put(v)` with `v: X` selects the `IL<X>` member `Put(x: X)` ([§22.6]).
//! cites: 11.1, 22.6
//! expect: run
//! output: 9
//!   8
//! issue: 6224
module Conf;

public interface IL<T>
  method Put(x: T): int;
  method Put(x: bool): int;
end IL;

public class Two: IL<int>, IL<string>
  public method Put(x: bool): int => 7;
  public method Put(x: string): int => 8;
  public method Put(x: int): int => 9;
end Two;

public static class S
  public static method Use<X>(i: IL<X>, v: X): int => i.Put(v);
end S;

public class P
  public static method Main(): int
    using var t := new Two();
    Console.WriteLine(S.Use<int>(t, 4));
    Console.WriteLine(S.Use<string>(t, "s"));
    return 0;
  end Main;
end P;
