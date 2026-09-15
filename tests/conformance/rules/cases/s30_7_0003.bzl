//! case: s30_7_0003
//! doc: spec
//! section: 30.7
//! title: with token passes a CancellationToken to GetAsyncEnumerator
//! rule: `await for` accepts an optional `with` clause to pass a
//!   `CancellationToken` to the async enumerator. The `with token` clause
//!   passes `token` as the argument to `GetAsyncEnumerator(token)`.
//! cites: 30.7
//! expect: accept
module Conf;

public class Reader
  public constructor()
  end constructor;

  public async method NumbersAsync(): owned IAsyncEnumerable<int>
    await Task.Delay(0);
    yield return 1;
  end NumbersAsync;
end Reader;

public class Program
  public async method RunAsync(token: CancellationToken): Task
    await for var n in new Reader().NumbersAsync() with token do
      Console.WriteLine(n);
    end;
  end RunAsync;
end Program;
