//! case: s30_2_0008
//! doc: spec
//! section: 30.2
//! title: a case binding captured by a lambda and read after an await in the same arm keeps its value
//! rule: "`await` suspends the current async method until a task completes ... If the task is running, the async method suspends; when the task completes, the continuation is queued" (§30.2, v0.340). A binding introduced by a `case` arm (§19.3) is a local of that arm, so its value must be the same after the resumption as before the suspension: `n` is 3 across the await, so the arm is `T(3) + 3 * 2` = 30 + 6.
//! cites: 19.3, 27.5
//! expect: run
//! stdout: 36
module Conf;

public class Program
  public static async method T(x: int): Task<int>
    await Task.Delay(1);
    return x * 10;
  end T;

  public static async method F(k: int): Task<int>
    var r := (case k of var n => (await T(n)) + (lambda(): int => n * 2)());
    return r;
  end F;

  public static async method Main(): Task<int>
    Console.WriteLine(await F(3));
    return 0;
  end Main;
end Program;
