// Probe f06 (proposal 0211 §5): a BINDING multi-type alternative that lists an
// interface. Spec §31.2.1 gives the binding "the nearest common base of the
// listed types — the most specific class that is a base of every listed type",
// and justifies it by "Every listed type is `Exception`-derived"; an interface
// is not. What does the binding's type come out as?
module F06;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Program
  public static method Main()
    try
      throw new BadInput("bad input");
    catch
      var ex: TimeoutException | IFailure => Console.WriteLine($"binding reads Message: {ex.Message}"),
    end try;
  end Main;
end Program;
