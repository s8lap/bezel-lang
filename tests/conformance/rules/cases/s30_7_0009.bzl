//! case: s30_7_0009
//! doc: spec
//! section: 30.7
//! title: an Object-typed await using var may not be live across a yield return
//! rule: An `await using var` whose static type is such a class (or is of
//!   **interface** static type ... or is `Object`: neither an interface nor
//!   `Object` can promise a destructor, and the disposal path is the run-time
//!   object's, so the runtime type's fallback posture is statically
//!   unknowable and such bindings are conservatively included ...) therefore
//!   **may not be live across a `yield return`** in an async iterator.
//! cites: 30.7, 28.12, C.6
//! expect: reject
//! diagnostic: yield
//! issue: 5654
module Conf;

public class Resource: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
  end DisposeAsync;

  destructor
    Console.WriteLine("sync cleanup");
  end destructor;
end Resource;

public class Reader
  public static async method StreamAsync(): owned IAsyncEnumerable<int>
    await using var o: Object := new Resource();
    yield return 1;
    Console.WriteLine(o.ToString());
    yield return 2;
  end StreamAsync;
end Reader;
