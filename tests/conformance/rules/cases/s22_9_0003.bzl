//! case: s22_9_0003
//! doc: spec
//! section: 22.9
//! title: an exception during the base constructor call leaves no derived state to clean up
//! rule: "An exception during the **base or chained constructor call**
//!   (step 1) leaves no derived state to clean up ... The exception
//!   propagates to the derived ctor's caller; the derived instance is
//!   never observable."
//! cites: 22.9, 22.8
//! expect: run
//! output: caught
module Conf;

public class Base
  public constructor()
    throw new InvalidOperationException("base fail");
  end constructor;
end Base;

public class A
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("A disposed");
  end destructor;
end A;

public class Derived: Base
  private owned _a: A := new A();

  public constructor()
  end constructor;
end Derived;

public class Program
  public static method Main()
    try
      using var d := new Derived();
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
