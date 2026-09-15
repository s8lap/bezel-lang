//! case: s22_9_0004
//! doc: spec
//! section: 22.9
//! title: a this-chaining body throw after the chain completes runs the normal dispose
//! rule: "If the chaining constructor's **body** throws, the chained-to
//!   construction completed and the instance is at that point fully
//!   initialized: the runtime disposes it through the normal
//!   generated-`Dispose()` path ... before the exception propagates."
//! cites: 22.9, 22.8
//! expect: run
//! output: Widget disposed
//!   A disposed
//!   caught
module Conf;

public class A
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("A disposed");
  end destructor;
end A;

public class Widget
  private owned _a: A := new A();

  public constructor()
  end constructor;

  public constructor(fail: bool)
    this();
    if fail then
      throw new InvalidOperationException("chained body fail");
    end if;
  end constructor;

  destructor
    Console.WriteLine("Widget disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    try
      using var w := new Widget(true);
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
