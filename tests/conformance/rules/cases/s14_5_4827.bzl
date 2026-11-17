//! case: s14_5_4827
//! doc: spec
//! section: 14.5
//! title: a bare K return position is bound from a string? target and the null lambda body is admitted
//! rule: A type parameter still unbound is bound from the call's **target type**, where the call
//!   site has one, by unifying the method's declared return type — current bindings applied —
//!   against the target
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
    var r: string? := Q.B(xs, lambda(v) => null);
    Console.WriteLine(r ?? "none");
  end Main;
end Program;
