module Probe;
public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    for var x in xs do
      xs.Add(x);
    end;
    Console.WriteLine(xs.Count);
  end Main;
end Program;
