module SmokeExceptions;

// A user exception thrown and caught (libunwind + libc++abi in the
// self-contained link), and a runtime-raised one.
public class MyError: Exception
  public constructor(msg: string)
    base(msg);
  end constructor;
end MyError;

public class Program
  private static method Boom(n: int)
    if n > 2 then
      throw new MyError($"too big {n}");
    end;
  end Boom;

  public static method Main()
    try
      Boom(5);
    catch
      var e: MyError => Console.WriteLine($"caught {e.Message}")
    end try;
    try
      var z := 0;
      Console.WriteLine($"{10 / z}");
    catch
      var e: DivideByZeroException => Console.WriteLine("div0")
    end try;
  end Main;
end Program;
