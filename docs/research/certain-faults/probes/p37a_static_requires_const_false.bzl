module Probe;
public class Program
  public static method Half(n: int): int
    static requires(n > 0)
    return n / 2;
  end Half;
  public static method Main()
    Console.WriteLine(Half(-4));
  end Main;
end Program;
