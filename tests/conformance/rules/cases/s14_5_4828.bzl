//! case: s14_5_4828
//! doc: spec
//! section: 14.5
//! title: a K bound by an earlier seed argument is completed, not overridden, by the target
//! rule: The target never overrides an argument-derived binding; it only completes.
//! cites: 14.5, 5.1
//! expect: run
//! stdout: none
//! issue: 8626
module Conf;

record Box(Name: string, Age: int);

public static class Q
  public static method F<T, K>(xs: Array<T>, key: (T) -> K?): K?
    return key(xs[0]);
  end F;

  public static method B<T, K>(xs: Array<T>, key: (T) -> K): K
    return key(xs[0]);
  end B;

  public static method S<T, K>(xs: Array<T>, seed: K?, key: (T) -> K?): K?
    return key(xs[0]);
  end S;
end Q;

public class Program
  public static method Main()
    var xs: Array<Box> := [new Box("ann", 30), new Box("dan", 35)];
    var r: string? := Q.S(xs, "s", lambda(v) => null);
    Console.WriteLine(r ?? "none");
  end Main;
end Program;
