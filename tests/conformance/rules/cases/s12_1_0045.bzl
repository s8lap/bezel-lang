//! case: s12_1_0045
//! doc: spec
//! section: 12.1
//! title: a concrete subclass overrides an abstract generic member at the substituted type
//! rule: `abstract` on a member declares it without a body; every concrete subclass must override it (v0.338, §12.5), and the override must match the base member exactly after substitution (v0.338, §12.1; the base clause instantiates `B<T>` with `T` = `Box<int>`, §14.1).
//! cites: 12.1, 12.5, 14.1
//! expect: run
//! output: 2
module Conf;

public class Box<T>
  public constructor() end constructor;
end Box;
public abstract class B<T>
  public constructor() end constructor;
  public abstract method Foo(x: T): int;
end B;
public class D: B<Box<int>>
  public constructor() end constructor;
  public override method Foo(x: Box<int>): int => 2;
end D;
public class Program
  public static method Main(): int
    // Trace: d is B<Box<int>> at runtime D. Its override of the abstract
    // member B<Box<int>>.Foo(x: Box<int>) is reached through the base type: 2.
    var d: B<Box<int>> := new D();
    Console.WriteLine(d.Foo(new Box<int>()));
    return 0;
  end Main;
end Program;
