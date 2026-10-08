//! case: s30_7_0008
//! doc: spec
//! section: 30.7
//! title: a class whose DisposeAsync level declares its own destructor may cross a yield
//! rule: For types **with** a destructor, the synchronous-fallback path above
//!   remains the defined degraded behavior, unchanged.
//! cites: 30.7, 28.12
//! expect: accept
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

  destructor
    Console.WriteLine("der destructor");
  end destructor;
end Der;

public class Reader
  public static async method StreamAsync(): owned IAsyncEnumerable<int>
    await using var d := new Der();
    yield return 1;
    d.Touch();
    yield return 2;
  end StreamAsync;
end Reader;
