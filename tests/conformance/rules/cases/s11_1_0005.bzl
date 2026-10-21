//! case: s11_1_0005
//! doc: spec
//! section: 11.1
//! title: a one-instantiation class answers a generic-body interface call through that instantiation
//! rule: "The class still declares **one** member set, and [§11.2]'s matching rule is applied once per obligation, each under its own instantiation's substitution" ([§11.1]); with the single obligation `IL<int>`, the call `i.Put(v)` with `v: X` selects `Put(x: X)` at `X = int` ([§22.6]).
//! cites: 11.1, 22.6
//! expect: run
//! stdout: 9
module Conf;

public interface IL<T>
  method Put(x: T): int;
end IL;

public class One: IL<int>
  public method Put(x: int): int => 9;
end One;

public static class S
  public static method Use<X>(i: IL<X>, v: X): int => i.Put(v);
end S;

public class P
  public static method Main(): int
    using var t := new One();
    Console.WriteLine(S.Use<int>(t, 4));
    return 0;
  end Main;
end P;
