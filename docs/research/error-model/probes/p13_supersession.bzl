// Probe p13: one in-flight exception slot, later-wins (spec §31.2, rationale
// §11.8). A cleanup that throws replaces the failure being propagated; the
// first one survives only as a snapshot in SupersededChain (not implemented in r40).
module P13;

public class Program
  public static method Work()
    try
      throw new FormatException("the real failure");
    finally
      throw new InvalidOperationException("cleanup failed");
    end try;
  end Work;

  public static method Main()
    try
      Program.Work();
    catch
      FormatException => Console.WriteLine("caught the real failure"),
      var ex: InvalidOperationException => do
        Console.WriteLine($"caught the cleanup failure: {ex.Message}");
        // spec §31.4 SupersededChain would hold a snapshot of the FormatException;
        // r40 does not implement the member ("no member `SupersededChain`"), so the
        // real failure is not recoverable here in this build.
      end,
    end try;
  end Main;
end Program;
