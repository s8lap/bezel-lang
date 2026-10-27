module Probe;
public class Program
  public static method Half(n: int): int
    static requires(n > 0)
    return n / 2;
  end Half;
  public static method Seven(): int => 7;
  public static method Main()
    Console.WriteLine(Half(Seven()));
  end Main;
end Program;
