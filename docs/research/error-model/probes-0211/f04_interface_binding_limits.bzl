// Probe f04 (proposal 0211 §3): what a binding of marker-interface type cannot do
// under today's rule (spec §31.2.1 binds the name at the arm's type). Both marked
// lines are expected to be rejected: the binding's static type is the interface,
// which has no `Message` and is not an `Exception`. r40 rejects (2) with its
// escape diagnostic, not a type error. (Passing the binding to an `Exception`
// parameter is f18: r40 accepts it, against spec §5.1.)
module F04;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Wrapped: Exception
  public constructor(msg: string, cause: Exception) base(msg, cause);
  end constructor;
end Wrapped;

public class Program
  public static method Main()
    try
      throw new BadInput("bad digits");
    catch
      var f: IFailure => do
        Console.WriteLine(f.Message);              // (1) no `Message` on the interface
        throw new Wrapped("lookup failed", f);     // (2) not an `Exception` cause
      end,
    end try;
  end Main;
end Program;
