module Conf;
// Family C: Pattern A await of a plain BORROWED task operand (§30.5's true-task-outlives-the-use residual). EXPECT: report at the await.
import System.Threading;
public class Data
  public constructor()
  end constructor;
  public method N(): int => 1;
end Data;
public class Program
  public static async method Report(t: Task<Data>): Task<int>
    var d := await t;
    return d.N();
  end Report;
end Program;
