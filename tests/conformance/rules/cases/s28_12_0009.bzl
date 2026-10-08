//! case: s28_12_0009
//! doc: spec
//! section: 28.12
//! title: a subclass inheriting DisposeAsync with its paired destructor is not warned
//! rule: A class that only inherits a `DisposeAsync` together with the
//!   destructor paired with it at that level has its fallback and is not
//!   warned.
//! cites: 28.12, 28.4
//! expect: quiet
module Conf;

public class Base: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("base async cleanup");
  end DisposeAsync;

  destructor
    Console.WriteLine("base destructor");
  end destructor;
end Base;

public class Der: Base
  public constructor()
  end constructor;
end Der;
