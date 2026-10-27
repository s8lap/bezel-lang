module Probe;
public enum Priority
  Low, Medium, High;
end Priority;
public class Program
  public static method Main()
    var p := Priority(99);
    Console.WriteLine(int(p));
  end Main;
end Program;
