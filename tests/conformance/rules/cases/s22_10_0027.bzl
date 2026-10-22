//! case: s22_10_0027
//! doc: spec
//! section: 22.10
//! title: a collection literal chained through this() reaches an owned primary-constructor parameter
//! rule: "the arguments of a `this(...)` call run before the chained-to primary's capture step" and may read only the constructor's own parameters, constants and statics (§22.10, v0.340); the literal `[5, 6, 7]` is a fresh producer passed by transfer to `owned c`, so `CL()` reads `3 + 100` = 103.
//! cites: 12.8, 28.8
//! expect: run
//! stdout: 103
module Conf;

public class C(owned c: Array<int>)
  private _k: int;

  public constructor(k: int)
    this([5, 6, 7]);
    _k := k;
  end;

  public method CL(): int => c.Count + _k;
end C;

public class Program
  public static method Main()
    var c := new C(100);
    Console.WriteLine(c.CL());
  end Main;
end Program;
