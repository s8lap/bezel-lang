module Conf;
// Family C control (0131 §3 row): `using var t := c.PingAsync(); await t;` awaited in scope. EXPECT: quiet.
import System.Threading;
public class Conn
  public constructor()
  end constructor;
  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;
public class Program
  public static async method Run(c: Conn): Task
    using var t := c.PingAsync();
    await t;
  end Run;
end Program;
