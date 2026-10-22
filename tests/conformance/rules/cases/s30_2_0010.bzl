//! case: s30_2_0010
//! doc: spec
//! section: 30.2
//! title: a case statement and a later case expression both binding n and awaiting do not interfere
//! rule: "`await` suspends the current async method until a task completes ... If the task is running, the async method suspends; when the task completes, the continuation is queued" (§30.2, v0.340). A binding introduced by a `case` arm (§19.3) is a local of that arm, so its value must be the same after the resumption as before the suspension: the statement arm gives `r` = 30 + 3 = 33 and the expression arm (its own `n` = 4) gives 40 + 4 = 44, so `33 * 1000 + 44`.
//! cites: 19.3
//! expect: run
//! stdout: 33044
module Conf;

public class Program
  public static async method T(x: int): Task<int>
    await Task.Delay(1);
    return x * 10;
  end T;

  public static async method F(k: int): Task<int>
    var r := 0;
    case k of
      var n => r := (await T(n)) + n;
    end;
    var q := (case k + 1 of var n => (await T(n)) + n);
    return r * 1000 + q;
  end F;

  public static async method Main(): Task<int>
    Console.WriteLine(await F(3));
    return 0;
  end Main;
end Program;
