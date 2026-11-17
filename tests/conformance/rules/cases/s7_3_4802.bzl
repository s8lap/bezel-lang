//! case: s7_3_4802
//! doc: spec
//! section: 7.3
//! title: a non-nullable string target binds K=string and a null lambda body cannot convert: compile error
//! rule: Assigning `null` to a non-nullable target is a compile error.
//! cites: 7.3, 14.5
//! expect: reject
//! diagnostic: null
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
    var r: string := Q.B(xs, lambda(v) => null);
    Console.WriteLine(r);
  end Main;
end Program;
