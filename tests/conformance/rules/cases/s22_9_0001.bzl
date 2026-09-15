//! case: s22_9_0001
//! doc: spec
//! section: 22.9
//! title: a constructor-body throw disposes the step-2 owned field, not the class's own destructor
//! rule: "An exception in the constructor body (step 3) triggers disposal
//!   of every owned field assigned during initialization (step 2) and the
//!   base subobject. ... the throwing class's `destructor` body is
//!   **never** executed on these edges."
//! cites: 22.9, 28.4
//! expect: run
//! stdout: A disposed
module Conf;

public class A
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("A disposed");
  end destructor;
end A;

public class Y
  private owned _a: A := new A();

  public constructor()
    throw new InvalidOperationException("boom");
  end constructor;

  destructor
    Console.WriteLine("Y disposed");
  end destructor;
end Y;

public class Program
  public static method Main()
    try
      using var y := new Y();
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
