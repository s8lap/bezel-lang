module Conf;
// Family C remedy: TaskScope.Spawn with a `scoped` body — the scope's disposal quiesces its children; a compiler-known bounding type. EXPECT: quiet.
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
    await using var scope := new TaskScope();
    scope.Spawn(lambda() => c.PingAsync());
    await scope.Join();
  end Run;
end Program;
