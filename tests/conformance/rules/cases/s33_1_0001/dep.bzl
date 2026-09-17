module Dep;

public class DepEntry
  public static method Main()
    Console.WriteLine("dep-a");
  end Main;

  public static method Main(args: Array<string>)
    Console.WriteLine("dep-b");
  end Main;
end DepEntry;
