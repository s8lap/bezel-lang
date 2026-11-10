// Probe f14 (proposal 0211 §6.6): supersession and the band. A `finally` that
// raises a failure while a bug is propagating replaces the bug (later wins,
// spec §31.2); the boundary's marker arm then takes the cleanup failure. The
// buried bug is in `SupersededChain` per spec §31.4; r40 does not implement
// that member (the note's p13), so this probe shows the arm selection only.
module F14;

public interface IFailure
end IFailure;

public class FlushFailed: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end FlushFailed;

public class Program
  static method Work()
    try
      using var xs := new Array<int>(2, 0);
      Console.WriteLine(xs[9]);                  // the bug
    finally
      throw new FlushFailed("flush failed");     // a failure raised during the unwind
    end try;
  end Work;

  public static method Main()
    try
      Program.Work();
    catch
      var f: IFailure => Console.WriteLine($"marker arm took: {f.ToString()}"),
      var ex: Exception => Console.WriteLine($"catch-all took: {ex.ToString()}"),
    end try;
  end Main;
end Program;
