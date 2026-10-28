module Conf;
// Family C: a minted task over a borrowed receiver stored into an owning collection field. EXPECT: report (the frame is held past the call).
import System.Threading;
public class Conn
  public constructor()
  end constructor;
  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;
public class Batch
  private owned _tasks: OwningArray<Task>;
  public constructor()
    _tasks := new OwningArray<Task>();
  end constructor;
  public method Start(c: Conn)
    _tasks.Add(c.PingAsync());
  end Start;
end Batch;
