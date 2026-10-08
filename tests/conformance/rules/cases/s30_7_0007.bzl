//! case: s30_7_0007
//! doc: spec
//! section: 30.7
//! title: an await using var whose class has only an inherited destructor may not cross a yield
//! rule: For a class **without a synchronous fallback** — some level of its
//!   chain declares or overrides a non-`abstract` `DisposeAsync` and declares
//!   **no destructor** of its own, a destructor it only inherits being a base
//!   level's cleanup ... An `await using var` whose static type is such a
//!   class ... therefore **may not be live across a `yield return`** in an
//!   async iterator: a compile error at the yield.
//! cites: 30.7, 28.12, C.6
//! expect: reject
//! diagnostic: yield
//! issue: 5654
module Conf;

public class Base
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("base destructor");
  end destructor;
end Base;

public class Der: Base, IAsyncDisposable
  public constructor()
  end constructor;

  public method Touch()
  end Touch;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
  end DisposeAsync;
end Der;

public class Reader
  public static async method StreamAsync(): owned IAsyncEnumerable<int>
    await using var d := new Der();
    yield return 1;
    d.Touch();
    yield return 2;
  end StreamAsync;
end Reader;
