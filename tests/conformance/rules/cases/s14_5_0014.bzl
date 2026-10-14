//! case: s14_5_0014
//! doc: spec
//! section: 14.5
//! title: a valid explicit type-argument list runs and selects the generic overload
//! rule: When a full explicit list is supplied, no inference runs.
//! cites: 14.5, 22.6
//! expect: run
//! stdout: 3
module Conf;

public class C
  public method S(x: string): int => 1;
  public method S<V>(x: Array<V>): int => x.Count;
end C;

public class Program
  public static method Main(): int
    using var c := new C();
    using var a: Array<int> := [1, 2, 3];
    Console.WriteLine(c.S<int>(a));
    return 0;
  end Main;
end Program;
