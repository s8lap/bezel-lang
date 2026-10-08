//! case: s28_12_0007
//! doc: spec
//! section: 28.12
//! title: await using at a base static type awaits the run-time object's DisposeAsync
//! rule: `await using` selects the logical-disposal path by the class of the
//!   object the binding holds — as the synchronous path selects the
//!   destructor chain it runs — never by the binding's static type ...
//!   `await using var b: Base := new Der();` with
//!   `class Der: Base, IAsyncDisposable` awaits `Der.DisposeAsync`, skips
//!   `Der`'s destructor and runs `Base`'s.
//! cites: 28.12, 28.4, C.6
//! expect: run
//! output: working
//!   der async cleanup
//!   base destructor
//!   done
//! issue: 5653
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

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("der async cleanup");
  end DisposeAsync;

  destructor
    Console.WriteLine("der destructor");
  end destructor;
end Der;

public class Program
  public static async method Main(): Task
    do
      await using var b: Base := new Der();
      Console.WriteLine("working");
    end;
    Console.WriteLine("done");
  end Main;
end Program;
