// Probe p04: an exception leaving a domain does not cross as itself. The join
// re-raises DomainFaultedException; the original type is not matchable and
// only strings cross (spec §30.14, stdlib §6.8, rationale §12.6).
module P04;

import System.Threading;
import System.IO;

public class Program
  public static method ReadConfig(): int
    throw new FileNotFoundException("app.toml");
  end ReadConfig;

  public static method BadIndex(): int
    var xs := new Array<int>(2, 0);
    return xs[5];
  end BadIndex;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.ReadConfig);
    try
      var n := await t;
      Console.WriteLine(n);
    catch
      FileNotFoundException => Console.WriteLine("matched FileNotFoundException"),
      IOException => Console.WriteLine("matched IOException"),
      var ex: DomainFaultedException => do
        Console.WriteLine($"matched DomainFaultedException: Message='{ex.Message}'");
        Console.WriteLine($"ToString: {ex.ToString()}");
      end,
    end try;

    // A bug in the domain arrives through the same door, the same type.
    using var u := Domain.Spawn(Program.BadIndex);
    try
      var n := await u;
      Console.WriteLine(n);
    catch
      IndexOutOfRangeException => Console.WriteLine("matched IndexOutOfRangeException"),
      var ex: DomainFaultedException => Console.WriteLine($"bug also arrives as DomainFaultedException: '{ex.Message}'"),
    end try;
  end Main;
end Program;
