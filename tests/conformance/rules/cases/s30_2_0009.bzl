//! case: s30_2_0009
//! doc: spec
//! section: 30.2
//! title: nested case expressions each awaiting keep both bindings across the suspensions
//! rule: "`await` suspends the current async method until a task completes ... If the task is running, the async method suspends; when the task completes, the continuation is queued" (§30.2, v0.340). A binding introduced by a `case` arm (§19.3) is a local of that arm, so its value must be the same after the resumption as before the suspension: both `a` and `b` survive: `T(3) + T(4) + 3 + 4` = 30 + 40 + 7.
//! cites: 19.3
//! expect: run
//! stdout: 77
module Conf;

public class Program
  public static async method T(x: int): Task<int>
    await Task.Delay(1);
    return x * 10;
  end T;

  public static async method F(k: int, j: int): Task<int>
    var r := (case k of var a => (case j of var b => (await T(a)) + (await T(b)) + a + b));
    return r;
  end F;

  public static async method Main(): Task<int>
    Console.WriteLine(await F(3, 4));
    return 0;
  end Main;
end Program;
