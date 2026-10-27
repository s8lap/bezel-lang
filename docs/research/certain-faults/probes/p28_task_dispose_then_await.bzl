module Probe;
public class Program
  public static async method Main(): Task
    var t := Task.FromResult<int>(5);
    t.Dispose();
    var v := await t;
    Console.WriteLine(v);
  end Main;
end Program;
