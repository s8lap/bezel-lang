//! case: s7_7_4705
//! doc: spec
//! section: 7.7
//! title: ??= admits a widening numeric right operand and a subclass right operand
//! rule: `x ??= e` assigns `e` only when `x`'s current value is null, with the typing and ownership rules
//!   of
//! cites: 7.7, 5.1
//! expect: run
//! output: 4000000000
//!   dog
module Conf;

public class Animal
  public constructor() end constructor;
  public virtual method Name(): string
    return "animal";
  end Name;
end Animal;

public class Dog: Animal
  public constructor() end constructor;
  public override method Name(): string
    return "dog";
  end Name;
end Dog;

public class Program
  public static method Main()
    var x: long? := null;
    var u: uint := 4000000000U;
    x ??= u;
    Console.WriteLine(x ?? 0L);
    var a: Animal? := null;
    a ??= new Dog();
    Console.WriteLine(a!.Name());
    a?.Dispose();
  end Main;
end Program;
