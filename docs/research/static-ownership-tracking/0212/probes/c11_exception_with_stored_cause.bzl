module Probe;
// CRITIQUE NOTE 11: an exception class that STORES its cause as a borrow, thrown from the catch arm that borrowed the cause. The caught exception is disposed after its arm (spec 31.1), so the stored cause is dead when the outer handler reads it. EXPECT today: compiles; a report line; checked = DisposedAccessException in the outer handler. RESULT on r40: compiles with NO report line, and prints an empty line with exit 0 in all three modes — the read of the dead cause is not caught even in checked mode. Under 0212: error at the `throw` — the operand carries the catch binding. c12 is the rewrite.
public class WrapError: Exception
  private _cause: Exception;
  public constructor(cause: Exception) base("wrapped");
    _cause := cause;
  end constructor;
  public method Cause(): string => _cause.Message;
end WrapError;
public class Program
  public static method Inner()
    throw new InvalidOperationException("boom");
  end Inner;
  public static method Outer()
    try
      Inner();
    catch e: InvalidOperationException =>
      throw new WrapError(e);
    end;
  end Outer;
  public static method Main(): int
    try
      Outer();
    catch w: WrapError =>
      Console.WriteLine(w.Cause());
    end;
    return 0;
  end Main;
end Program;
