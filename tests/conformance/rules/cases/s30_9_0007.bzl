//! case: s30_9_0007
//! doc: spec
//! section: 30.9
//! title: a using var operand whose fault escapes is disposed at the faulted task's teardown
//! rule: A `using var`/`await using var` operand's disposal is **deferred** to
//!   the faulted task's teardown, as axiom A12 defers a chained source's.
//! cites: 30.9, 30.5
//! expect: run
//! output: boom
//!   boom
//!   instance disposed
//!   done
//! issue: 5831
module Conf;

public class BoomException: Exception
  public constructor(message: string) base(message);
  end constructor;

  destructor
    Console.WriteLine("instance disposed");
  end destructor;
end BoomException;

public class Demo
  public static async method Inner(): Task
    await Task.Delay(10);
    throw new BoomException("boom");
  end Inner;

  public static async method Outer(): Task
    using var inner := Inner();
    await inner;
  end Outer;

  public static async method Main(): Task
    do
      using var o := Outer();
      for var i in 1..2 do
        try
          await o;
        catch
          var e: BoomException => Console.WriteLine(e.Message)
        end try;
      end;
    end;
    Console.WriteLine("done");
  end Main;
end Demo;
