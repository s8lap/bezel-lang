//! case: s28_12_0008
//! doc: spec
//! section: 28.12
//! title: a DisposeAsync level whose only destructor is inherited draws the no-destructor warning
//! rule: A class is **without a synchronous fallback** when any level of its
//!   chain declares or overrides a non-`abstract` `DisposeAsync` and declares
//!   no destructor of its own ... The compiler emits a warning at such a
//!   level's declaration — a class that declares or overrides a
//!   non-`abstract` `DisposeAsync` and declares no destructor of its own.
//! cites: 28.12, 28.4
//! expect: warn
//! diagnostic: skip async cleanup
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

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("der async cleanup");
  end DisposeAsync;
end Der;
