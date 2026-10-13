//! case: s5_1_0116
//! doc: spec
//! section: 5.1
//! title: a class reference converts implicitly to a base class two levels up
//! rule: §5.1 (reference conversions): "class to base class, transitively, up to `Object`".
//!   C derives from B, which derives from A; the conversion of a C reference to A is implicit, and the call through A reaches A's method.
//! cites: 5.1
//! expect: run
//! stdout: 7
module Conf;

public class A
  public constructor() end constructor;

  public method Id(): int
    return 7;
  end Id;
end A;

public class B: A
  public constructor() end constructor;
end B;

public class D: B
  public constructor() end constructor;
end D;

public class C
  public static method Main(): int
    var a: A := new D();
    Console.WriteLine(a.Id());
    return 0;
  end Main;
end C;
