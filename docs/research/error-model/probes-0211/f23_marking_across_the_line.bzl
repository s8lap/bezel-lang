// Probe f23 (proposal 0211 §3.3): what user code can declare today, and the
// proposal rejects. A class deriving from `OperationCanceledException`, and one
// deriving from `InvalidOperationException`, each list the marker; a marker arm
// then takes a cancellation, and an `InvalidOperationException` arm takes a
// "failure". Both compile on r40.
module F23;

public interface IFailure
end IFailure;

public class SoftCancel: OperationCanceledException, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end SoftCancel;

public class Stale: InvalidOperationException, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end Stale;

public class Program
  public static method Main()
    try
      throw new SoftCancel("cancelled");
    catch
      IFailure => Console.WriteLine("a marker arm took a cancellation"),
    end try;
    try
      throw new Stale("stale");
    catch
      InvalidOperationException => Console.WriteLine("a state-defect arm took a class marked as a failure"),
    end try;
  end Main;
end Program;
