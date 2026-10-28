module Conf;
// Family C: the SPEC'S OWN §30.1 example — a static async method whose plain ARGUMENT `c` is held in the frame, abandoned by Task.FireAndForget. EXPECT: report.
import System.Threading;
public class Conn
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Conn;
public class Program
  public static async method ShowAsync(c: Conn): Task
    await Task.Delay(1);
    Console.WriteLine(c.Name);
  end ShowAsync;
  public static method Fire(c: Conn)
    Task.FireAndForget(ShowAsync(c));
  end Fire;
end Program;
