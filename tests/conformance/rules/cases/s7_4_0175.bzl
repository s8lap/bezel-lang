//! case: s7_4_0175
//! doc: spec
//! section: 7.4
//! title: a narrowing fact on a readonly function-typed field survives an await
//! rule: no call, `await`, or closure can invalidate a fact about its value
//! cites: 30.13, 30.1
//! expect: run
//! output: 2
//!   none
//! issue: 8099
module Conf;

public class K
  private readonly _f: ((int) -> int)?;

  public constructor(present: bool)
    if present then
      _f := lambda(x: int) => x + 1;
    else
      _f := null;
    end if;
  end constructor;

  public async method Fire(): Task
    if this._f is (int) -> int then
      await Task.Delay(1);
      Console.WriteLine(this._f(1));
    else
      Console.WriteLine("none");
    end if;
  end Fire;
end K;

public class Program
  public static async method Main(): Task<int>
    using var a := new K(true);
    await a.Fire();
    using var b := new K(false);
    await b.Fire();
    return 0;
  end Main;
end Program;
