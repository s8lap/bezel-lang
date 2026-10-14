//! case: s12_1_0044
//! doc: spec
//! section: 12.1
//! title: two instantiations of one generic base dispatch to their own overrides
//! rule: To override a virtual member in a derived class, mark the matching member `override`. Visibility, return type, parameter types, and parameter modifiers must match the base member exactly (v0.338, §12.1); each base clause instantiates `B<T>` at its own argument (§14.1), so `B<int>` and `B<string>` have distinct members, and a class that does not override a member runs the base member.
//! cites: 12.1, 14.1
//! expect: run
//! output: 2
//!   3
//!   1
module Conf;

public class B<T>
  public constructor() end constructor;
  public virtual method Foo(x: T): int => 1;
end B;
public class D: B<int>
  public constructor() end constructor;
  public override method Foo(x: int): int => 2;
end D;
public class F: B<string>
  public constructor() end constructor;
  public override method Foo(x: string): int => 3;
end F;
public class Program
  public static method Main(): int
    // Trace: b1 runs D.Foo (2); b2 runs F.Foo (3); b3 is a plain B<bool>
    // with no override, so the base body runs (1).
    var b1: B<int> := new D();
    var b2: B<string> := new F();
    var b3: B<bool> := new B<bool>();
    Console.WriteLine(b1.Foo(1));
    Console.WriteLine(b2.Foo("s"));
    Console.WriteLine(b3.Foo(true));
    return 0;
  end Main;
end Program;
