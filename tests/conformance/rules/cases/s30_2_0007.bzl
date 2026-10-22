//! case: s30_2_0007
//! doc: spec
//! section: 30.2
//! title: an await inside a case expression arm over a unit scrutinee yields the awaited value
//! rule: "`await` suspends the current async method until a task completes ... If the task is running, the async method suspends; when the task completes, the continuation is queued" (§30.2, v0.340). A binding introduced by a `case` arm (§19.3) is a local of that arm, so its value must be the same after the resumption as before the suspension: the arm's value is `await T(k)`, `3 * 10`.
//! cites: 4.7, 19.3
//! expect: run
//! stdout: 30
module Conf;

public class Program
  public static async method T(x: int): Task<int>
    await Task.Delay(1);
    return x * 10;
  end T;

  public static async method F(k: int): Task<int>
    var u: () := ();
    var r := (case u of var x => (await T(k)));
    return r;
  end F;

  public static async method Main(): Task<int>
    Console.WriteLine(await F(3));
    return 0;
  end Main;
end Program;
