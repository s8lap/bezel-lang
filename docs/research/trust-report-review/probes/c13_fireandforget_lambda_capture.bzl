module Conf;
// Family C: an async lambda capturing a plain parameter, invoked and abandoned. EXPECT: report (the capture-set contribution rides the frame).
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
    var body := async lambda(): Task => do await c.PingAsync(); end;
    Task.FireAndForget(body());
  end Fire;
end Program;
