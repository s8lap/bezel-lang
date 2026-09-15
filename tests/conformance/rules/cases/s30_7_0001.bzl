//! case: s30_7_0001
//! doc: spec
//! section: 30.7
//! title: await for consumes an async iterator's yielded values in order
//! rule: `await for` is valid only inside an async method or async lambda.
//!   It desugars analogously to `for`: it acquires an async enumerator with
//!   `await using`, calls `MoveNextAsync()` with `await` in a loop.
//! cites: 30.7
//! expect: run
//! stdout: 6
module Conf;

public class Reader
  public constructor()
  end constructor;

  public async method NumbersAsync(): owned IAsyncEnumerable<int>
    await Task.Delay(0);
    yield return 1;
    yield return 2;
    yield return 3;
  end NumbersAsync;
end Reader;

public class Program
  public static async method Main(): Task
    var total := 0;
    await for var n in new Reader().NumbersAsync() do
      total := total + n;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
