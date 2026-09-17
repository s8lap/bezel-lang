module Dep;

public class Worker
  public static method Main()
    Console.WriteLine("dep");
  end Main;

  public static method Label(): string => "worker";
end Worker;
