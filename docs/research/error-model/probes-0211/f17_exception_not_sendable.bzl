// Probe f17 (proposal 0211 §6.3): implementing the marker makes no exception
// class sendable. Spec §30.14: "no exception class is sendable". Expected:
// rejected at the `Domain.Spawn` call.
module F17;

import System.Threading;

public interface IFailure
end IFailure;

public sealed class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Program
  static method Use(owned e: BadInput)
    Console.WriteLine(e.Message);
  end Use;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Use, new BadInput("x"));
    await t;
  end Main;
end Program;
