// Probe f03 (proposal 0211 §3): the same shape as f02 with `Exception` as the
// base class: a marker interface that declares the reporting surface, and an
// exception class that lists it and declares nothing.
module F03;

public interface IFailure
  readonly property Message: string;
  readonly property StackTrace: string;
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Program
  public static method Main()
    try
      throw new BadInput("bad digits");
    catch
      var f: IFailure => Console.WriteLine($"Message through the marker: {f.Message}"),
    end try;
  end Main;
end Program;
