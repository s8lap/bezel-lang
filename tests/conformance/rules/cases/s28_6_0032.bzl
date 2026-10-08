//! case: s28_6_0032
//! doc: spec
//! section: 28.6
//! title: base.DisposeAsync() directly in a DisposeAsync override is admitted as the chain
//! rule: In the body of a `DisposeAsync` override itself, not in a lambda
//!   nested in it, `base.DisposeAsync()` is §28.12 step 2's chain, not a
//!   disposal of `this`, and is admitted.
//! cites: 28.6, 28.12
//! expect: accept
module Conf;

public class Base: IAsyncDisposable
  public constructor()
  end constructor;

  public async virtual method DisposeAsync(): Task
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

  public async override method DisposeAsync(): Task
    Console.WriteLine("der async cleanup");
    await base.DisposeAsync();
  end DisposeAsync;

  destructor
    Console.WriteLine("der destructor");
  end destructor;
end Der;
