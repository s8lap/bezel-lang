module Conf;
// Family C: a minted task STORED into an owned field — the frame outlives the call with a borrowed receiver (§28.3 report text: "a task ... stored"). EXPECT: report.
import System.Threading;
public class Conn
  public constructor()
  end constructor;
  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;
public class Monitor
  private owned _pending: Task;
  public constructor(c: Conn)
    _pending := c.PingAsync();
  end constructor;
end Monitor;
