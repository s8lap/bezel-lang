module Probe;
// CRITIQUE NOTE 11, the rewrite: the exception OWNS what it keeps. Here it keeps the cause's message, a string; a class that must keep the cause object declares the field `owned` and is given an owned exception. (The standard exceptions' `inner` constructor copies a snapshot into the cause chain and retains no reference, stdlib 2 and spec 31.4.) EXPECT: compiles; no report line; clean in both modes; prints boom.
public class WrapError: Exception
  private _cause: string;
  public constructor(cause: Exception) base("wrapped");
    _cause := cause.Message;
  end constructor;
  public method Cause(): string => _cause;
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
