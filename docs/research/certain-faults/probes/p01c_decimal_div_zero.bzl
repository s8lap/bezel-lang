module Probe;
public class Program
  public static method Seven(): decimal => 7m;
  public static method Main()
    var d := Seven();
    Console.WriteLine(d / 0m);
  end Main;
end Program;
