//! case: s22_9_0002
//! doc: spec
//! section: 22.9
//! title: an exception in a field initializer disposes earlier-completed owned fields in reverse order
//! rule: "An exception during a **field initializer** (step 2) triggers
//!   disposal of any earlier derived owned fields whose initializers
//!   completed, in reverse declaration order, followed by the base
//!   subobject's `Dispose()`."
//! cites: 22.9, 22.8
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

public class Fails
  public constructor()
    throw new InvalidOperationException("bad");
  end constructor;
end Fails;

public class Y
  private owned _first: A := new A();
  private owned _second: Fails := new Fails();

  public constructor()
  end constructor;
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
