// Probe p03: an exception thrown after an await faults the task; the frame's
// using var is disposed on the fault edge; awaiting re-raises the task-owned
// instance, and a second awaiter of the same Task sees the same fault
// (spec §30.9, §30.4).
module P03;

public class Res
  private _name: string;
  public constructor(name: string)
    _name := name;
    Console.WriteLine($"  open {name}");
  end constructor;
  destructor
    Console.WriteLine($"  close {_name}");
  end destructor;
end Res;

public class Program
  public static async method Step(): Task
    await Task.Delay(1);
  end Step;

  public static async method Fetch(fail: bool): Task<int>
    using var conn := new Res("conn");
    await Program.Step();
    if fail then
      throw new FormatException("bad reply");
    end if;
    return 7;
  end Fetch;

  public static async method Main(): Task
    Console.WriteLine("await in try:");
    try
      var n := await Program.Fetch(true);
      Console.WriteLine(n);
    catch
      var ex: FormatException => Console.WriteLine($"  caught {ex.Message}"),
    end try;

    Console.WriteLine("two awaiters of one faulted task:");
    using var t := Program.Fetch(true);
    for var i in 1..2 do
      try
        var n := await t;
        Console.WriteLine(n);
      catch
        var ex: FormatException => Console.WriteLine($"  awaiter {i} caught {ex.Message}"),
      end try;
    end for;
    Console.WriteLine($"  IsFaulted = {t.IsFaulted}");
  end Main;
end Program;
