module Conf;
// 0131 §1.2 exclusion: a frame returned under a mint-announcing type is type-announced. EXPECT: quiet.
import System.Threading;
public class Conn
  public constructor()
  end constructor;
  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;
public class Program
  public static method Ping(c: Conn): Task => c.PingAsync();
end Program;
