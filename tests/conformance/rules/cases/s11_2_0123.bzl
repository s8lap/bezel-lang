//! case: s11_2_0123
//! doc: spec
//! section: 11.2
//! title: a class inherits interface conformance from a base class two levels up
//! rule: §5.1 lists as implicit "a type to an interface it implements or conforms to: classes by declaration or inheritance (§11.2…)".
//!   Class D derives from B, which derives from A, and A declares the matching public member P, so D reaches I through inheritance.
//! cites: 11.2, 5.1
//! expect: run
//! stdout: 42
module Conf;

public interface I
  method P(): int;
end I;

public class A: I
  public constructor() end constructor;

  public method P(): int
    return 42;
  end P;
end A;

public class B: A
  public constructor() end constructor;
end B;

public class D: B
  public constructor() end constructor;
end D;

public class C
  public static method Main(): int
    var s: I := new D();
    Console.WriteLine(s.P());
    return 0;
  end Main;
end C;
