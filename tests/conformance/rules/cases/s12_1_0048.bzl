//! case: s12_1_0048
//! doc: spec
//! section: 12.1
//! title: an override of one generic-substituted overload leaves the sibling overload on its own slot
//! rule: To override a virtual member in a derived class, mark the matching member `override`. Visibility, return type, parameter types, and parameter modifiers must match the base member exactly (v0.338, §12.1); the base clause `B<Box<string>>` instantiates `B<T>` at `T` = `Box<string>`, which leaves two overloads of distinct parameter types (`Foo(x: Box<string>)` and `Foo(x: Item)`) in the base; overload selection among them is by exact applicability over the substituted signatures (v0.338, §22.6).
//! cites: 12.1, 14.1, 22.6
//! expect: run
//! output: 2
//!   5
//!   1
module Conf;

public class Item
  public constructor() end constructor;
end Item;
public class Box<T>
  public constructor() end constructor;
end Box;
public class B<T>
  public constructor() end constructor;
  public virtual method Foo(x: T): int => 1;
  public virtual method Foo(x: Item): int => 5;
end B;
public class D: B<Box<string>>
  public constructor() end constructor;
  public override method Foo(x: Box<string>): int => 2;
end D;
public class Program
  public static method Main(): int
    // Trace: d is B<Box<string>> at runtime D. Foo(Box<string>) is D's override
    // (2); Foo(Item) is not overridden and runs the base body (5). A plain
    // B<Box<string>> would print 1 for Foo(Box<string>), shown last.
    var d: B<Box<string>> := new D();
    var b: B<Box<string>> := new B<Box<string>>();
    Console.WriteLine(d.Foo(new Box<string>()));
    Console.WriteLine(d.Foo(new Item()));
    Console.WriteLine(b.Foo(new Box<string>()));
    return 0;
  end Main;
end Program;
