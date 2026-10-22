//! case: s30_7_0012
//! doc: spec
//! section: 30.7
//! title: a case expression awaiting inside an async iterator keeps its binding across yield and await
//! rule: "`await` suspends the current async method until a task completes" (§30.2, v0.340) and an async iterator may `await` between `yield return`s (§30.7, v0.340); a `case` arm's binding keeps its value across both: `q` = `T(3) + 3` = 33, then `z` = 33 gives `T(33) + 33` = 363.
//! cites: 30.2, 19.3
//! expect: run
//! output: 33
//!   363
module Conf;

public class Program
  public static async method T(x: int): Task<int>
    await Task.Delay(1);
    return x * 10;
  end T;

  public static async method It(k: int): IAsyncEnumerable<int>
    var q := (case k of var n => (await T(n)) + n);
    yield return q;
    await Task.Delay(1);
    yield return (case q of var z => (await T(z)) + z);
  end It;

  public static async method Main(): Task<int>
    await for var v in It(3) do Console.WriteLine(v); end;
    return 0;
  end Main;
end Program;
