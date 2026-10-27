module Probe;

public sealed class E: Exception
  public constructor()
    base("E");
  end constructor;
  destructor
    Console.WriteLine("D:E");
  end destructor;
end E;

public class Program
  public static method Main(): int
    throw new E();                    // §31.1: disposed "after the exception terminates the program if uncaught"; D:E is never printed
  end Main;
end Program;
