module Probe;
public class Program
  public static method Seven(): int => 7;
  public static method Main()
    var x := Seven();
    Console.WriteLine(x / 0);
  end Main;
end Program;
