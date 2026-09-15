//! case: s30_7_0006
//! doc: spec
//! section: 30.7
//! title: an await using var may not be live across a yield return
//! rule: An `await using var` whose static type is such a class (or is of
//!   **interface** static type) ... may not be **live across a `yield
//!   return`** in an async iterator: a compile error at the yield.
//! cites: 30.7, 28.12
//! expect: reject
//! diagnostic: yield
module Conf;

public class Resource: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
  end DisposeAsync;
end Resource;

public class Reader
  public constructor()
  end constructor;

  public async method StreamAsync(): owned IAsyncEnumerable<int>
    await using var r := new Resource();
    yield return 1;
    yield return 2;
  end StreamAsync;
end Reader;
