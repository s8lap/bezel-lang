//! case: s30_7_0010
//! doc: spec
//! section: 30.7
//! title: the same binding at its own class static type, which declares a destructor, may cross a yield
//! rule: A binding whose static type is any other class is admitted, even
//!   where it holds an object of a subclass without a synchronous fallback.
//! cites: 30.7, 28.12
//! expect: accept
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
    await using var o: Resource := new Resource();
    yield return 1;
    Console.WriteLine(o.ToString());
    yield return 2;
  end StreamAsync;
end Reader;
