module XLC;

public class MyError(message: string): Exception(message)
  public constructor() this("default");
    Console.WriteLine("impure convenience");
  end constructor;
end MyError;
