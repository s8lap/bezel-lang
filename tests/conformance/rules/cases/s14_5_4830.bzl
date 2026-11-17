//! case: s14_5_4830
//! doc: spec
//! section: 14.5
//! title: no target and no self-determined position leaves K unbound: compile error
//! rule: A type parameter that remains unbound — it appears in no self-determined argument
//!   position and no target context supplies it — is a compile error naming the parameter
//! cites: 14.5, 5.1
//! expect: reject
//! diagnostic: cannot infer
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
    var r := Q.F(xs, lambda(v) => null);
    Console.WriteLine("x");
  end Main;
end Program;
