//! case: s12_1_0047
//! doc: spec
//! section: 12.1
//! title: an override may fix one base argument and keep a type parameter in another
//! rule: To override a virtual member in a derived class, mark the matching member `override`. Visibility, return type, parameter types, and parameter modifiers must match the base member exactly (v0.338, §12.1); the base clause `B<V, int>` instantiates `B<T, U>` with `T` = `V` and `U` = `int`, so the base member's parameters are `(x: V, y: int)` (§14.1).
//! cites: 12.1, 14.1
//! expect: run
//! output: 2
//!   3
module Conf;

public class B<T, U>
  public constructor() end constructor;
  public virtual method Foo(x: T, y: U): int => 1;
end B;
public class D<V>: B<V, int>
  public constructor() end constructor;
  public override method Foo(x: V, y: int): int => 2;
end D;
public class E: B<int, int>
  public constructor() end constructor;
  public override method Foo(x: int, y: int): int => 3;
end E;
public class Program
  public static method Main(): int
    // Trace: d is B<string, int> at runtime D<string>; its override matches
    // B<string, int>.Foo(x: string, y: int) and prints 2. e is B<int, int>
    // at runtime E, and E.Foo(x: int, y: int) prints 3.
    var d: B<string, int> := new D<string>();
    var e: B<int, int> := new E();
    Console.WriteLine(d.Foo("s", 1));
    Console.WriteLine(e.Foo(1, 1));
    return 0;
  end Main;
end Program;
