//! case: s12_1_0043
//! doc: spec
//! section: 12.1
//! title: a three-level override chain matches each level's substituted parameter
//! rule: To override a virtual member in a derived class, mark the matching member `override`. Visibility, return type, parameter types, and parameter modifiers must match the base member exactly, and an override matches against the nearest overridden declaration (v0.338, §12.1); each base clause instantiates its generic base (§14.1).
//! cites: 12.1, 14.1
//! expect: run
//! output: 3
//!   2
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;
public class A<T>
  public constructor() end constructor;
  public virtual method Foo(x: T): int => 1;
end A;
public class B<U>: A<Box<U>>
  public constructor() end constructor;
  public override method Foo(x: Box<U>): int => 2;
end B;
public class C: B<int>
  public constructor() end constructor;
  public override method Foo(x: Box<int>): int => 3;
end C;
public class Program
  public static method Main(): int
    // Trace: a is A<Box<int>> at runtime C. Its nearest overridden declaration
    // is B<int>.Foo(x: Box<int>), which C overrides: prints 3.
    // c is A<Box<string>> at runtime B<string>: prints 2 via B<string>'s override.
    var a: A<Box<int>> := new C();
    Console.WriteLine(a.Foo(new Box<int>()));
    var c: A<Box<string>> := new B<string>();
    Console.WriteLine(c.Foo(new Box<string>()));
    return 0;
  end Main;
end Program;
