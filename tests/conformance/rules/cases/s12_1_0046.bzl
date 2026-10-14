//! case: s12_1_0046
//! doc: spec
//! section: 12.1
//! title: an override of a nested generic parameter matches the substituted base member
//! rule: To override a virtual member in a derived class, mark the matching member `override`. Visibility, return type, parameter types, and parameter modifiers must match the base member exactly (v0.338, §12.1); the base clause `B<string>` instantiates `B<T>`, so the base parameter type `Box<T>` is `Box<string>` (§14.1).
//! cites: 12.1, 14.1
//! expect: run
//! output: 2
//!   1
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;
public class B<T>
  public constructor() end constructor;
  public virtual method Foo(x: Box<T>): int => 1;
end B;
public class D: B<string>
  public constructor() end constructor;
  public override method Foo(x: Box<string>): int => 2;
end D;
public class Program
  public static method Main(): int
    // Trace: d is B<string> at runtime D: its override matches
    // B<string>.Foo(x: Box<string>), prints 2. c is an un-overridden
    // B<int>, so the base body runs and prints 1.
    var d: B<string> := new D();
    var c: B<int> := new B<int>();
    Console.WriteLine(d.Foo(new Box<string>()));
    Console.WriteLine(c.Foo(new Box<int>()));
    return 0;
  end Main;
end Program;
