module Conf;
// Family C: an instance async method on a field-sourced receiver with a plain-parameter ARGUMENT, abandoned. EXPECT: report (the argument `c` is a held source even though the receiver is `this`-rooted).
import System.Threading;
public class Conn
  public constructor()
  end constructor;
end Conn;
public class Pinger
  public async method PingAsync(c: Conn): Task
    await Task.Delay(1);
  end PingAsync;
  public method Fire(c: Conn)
    Task.FireAndForget(this.PingAsync(c));
  end Fire;
end Pinger;
