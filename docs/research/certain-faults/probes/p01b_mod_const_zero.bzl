module Probe;
public class Program
  public const Z: int := 0;
  public static method Seven(): int => 7;
  public static method Main()
    var x := Seven();
    Console.WriteLine(x mod Z);
  end Main;
end Program;
