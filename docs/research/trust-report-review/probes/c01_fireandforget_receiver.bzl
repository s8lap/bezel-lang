module Conf;
// Family C (s28_3_0095): Task.FireAndForget of a task minted over a plain-parameter RECEIVER. EXPECT: report.
import System.Threading;
public class Conn
  public constructor()
  end constructor;
  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;
public class Program
  public static method Fire(c: Conn)
    Task.FireAndForget(c.PingAsync());
  end Fire;
end Program;
