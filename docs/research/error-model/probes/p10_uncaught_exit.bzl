// Probe p10: an uncaught exception unwinds (disposals run), is reported, and
// the process exits 70 (spec §33.3). Nothing in a signature warned the caller.
module P10;

public class Res
  public constructor() Console.WriteLine("open"); end constructor;
  destructor
    Console.WriteLine("close (ran during the uncaught unwind)");
  end destructor;
end Res;

public class Program
  public static method Main()
    using var r := new Res();
    var n := int.Parse("12x");
    Console.WriteLine(n);
  end Main;
end Program;
