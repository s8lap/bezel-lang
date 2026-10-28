module Conf;
// Family C: a task bound and NOT awaited on every path before a held source's scope exits (0131 Q3: spec-ahead). EXPECT: report.
import System.Threading;
public class Conn
  public constructor()
  end constructor;
  public async method PingAsync(): Task
    await Task.Delay(1);
  end PingAsync;
end Conn;
public class Program
  public static async method Maybe(flag: bool): Task
    using var c := new Conn();
    using var t := c.PingAsync();
    if flag then
      await t;
    end;
  end Maybe;
end Program;
