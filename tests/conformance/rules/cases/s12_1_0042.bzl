//! case: s12_1_0042
//! doc: spec
//! section: 12.1
//! title: an override in a generic derived class matches the substituted base parameter
//! rule: To override a virtual member in a derived class, mark the matching member `override`. Visibility, return type, parameter types, and parameter modifiers must match the base member exactly (v0.338, §12.1); the base clause `B<Box<U>>` instantiates `B<T>` with `T` = `Box<U>`, so the base member's parameter type is `Box<U>` (§14.1).
//! cites: 12.1, 14.1
//! expect: run
//! output: 3
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;
public class B<T>
  public constructor() end constructor;
  public virtual method Foo(x: T): int => 1;
end B;
public class E<U>: B<Box<U>>
  public constructor() end constructor;
  public override method Foo(x: Box<U>): int => 3;
end E;
public class Program
  public static method Main(): int
    // Trace: e is typed B<Box<string>>; its runtime class is E<string>, whose
    // override matches B<Box<string>>.Foo(x: Box<string>). The call prints 3.
    var e: B<Box<string>> := new E<string>();
    Console.WriteLine(e.Foo(new Box<string>()));
    return 0;
  end Main;
end Program;
