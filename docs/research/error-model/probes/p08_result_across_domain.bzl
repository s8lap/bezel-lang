// Probe p08: a value-like Result is sendable (spec §35.12), so a typed
// failure crosses a domain seam as a value, with its type intact, where an
// exception arrives as DomainFaultedException (probe p04).
module P08;

import System.Threading;

public record LoadError(Code: int, Path: string);

public class Program
  public static method Load(): Result<int, LoadError>
    return Error(new LoadError(2, "app.toml"));
  end Load;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Load);
    case await t of
      Ok(var n)    => Console.WriteLine(n),
      Error(var e) => Console.WriteLine($"typed failure crossed the seam: code {e.Code}, path {e.Path}")
    end case;
  end Main;
end Program;
