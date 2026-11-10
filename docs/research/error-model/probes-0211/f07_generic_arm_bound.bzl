// Probe f07 (proposal 0211 §5): a generic catch-arm parameter and the marker.
// Spec §31.2.1 requires an `Exception`-derived class bound on a type parameter
// used as an arm type. (1) `where T: Exception, IFailure` — a class bound plus
// the marker; (2) the marker alone is in f08 (expected: rejected).
module F07;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Check
  public static method Fails<T>(body: () -> ()): string?
      where T: Exception, IFailure
    try
      body();
    catch
      var ex: T => return ex.Message;
    end try;
    return null;
  end Fails;
end Check;

public class Program
  public static method Main()
    var m := Check.Fails<BadInput>(lambda() => do throw new BadInput("bad input"); end);
    Console.WriteLine($"(1) class bound + marker: {m ?? "null"}");
  end Main;
end Program;
