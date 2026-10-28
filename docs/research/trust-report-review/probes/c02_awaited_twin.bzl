module Conf;
// Family C control (s28_3_0096): awaited in the sources' scope. EXPECT: quiet.
import System.Threading;
public class Conn
  public constructor()
  end constructor;
  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;
public class Program
  public static async method Fire(c: Conn): Task
    await c.PingAsync();
  end Fire;
end Program;
