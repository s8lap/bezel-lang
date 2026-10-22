//! case: s14_1_0004
//! doc: spec
//! section: 14.1
//! title: a derived class calls its constructed generic base's members by bare name, also in a
//!   lambda
//! rule: "An instantiation of a generic declaration is the constructed type or constructed
//!   member obtained by substituting type arguments for its type parameters" (§14.1,
//!   v0.340). `Sub : Box<string>` inherits `Get(): T` and `Get(x: int): T` instantiated at
//!   `string`, so the bare call `Get().Length` and `Get(n).Length` type-check and run,
//!   including inside a lambda in `Sub`, and so does `base.Get()`.
//! cites: 14.2, 27.3
//! expect: run
//! output: 10
//!   5
//!   10
//!   6
module Conf;

public class Box<T>
  public V: T;

  public constructor(v: T)
    V := v;
  end constructor;

  public method Get(): T => V;
  public method Get(x: int): T => V;
end Box;

public class Sub : Box<string>
  public constructor(v: string)
    base(v);
  end constructor;

  public method Get2(): int => 5;
  public method L(): int => Get2() + Get().Length;
  public method L2(): int => base.Get().Length;

  public method LL(): int
    var f := lambda() => Get().Length;
    return f() + Get().Length;
  end LL;

  public method LN(): int
    var g := lambda(n: int) => Get(n).Length + n;
    return g(1);
  end LN;
end Sub;

public class Program
  public static method Main()
    using var s := new Sub("hello");
    Console.WriteLine(s.L());
    Console.WriteLine(s.L2());
    Console.WriteLine(s.LL());
    Console.WriteLine(s.LN());
  end Main;
end Program;
