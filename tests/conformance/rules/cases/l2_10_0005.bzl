//! case: l2_10_0005
//! doc: stdlib
//! section: 2.10
//! title: IAsyncDisposable is implemented explicitly with an async DisposeAsync
//! rule: IAsyncDisposable is implemented explicitly by classes that need
//!   asynchronous cleanup; implementations declare a method matching
//!   DisposeAsync(): Task and add async -- async never appears on the
//!   interface declaration itself.
//! expect: accept
module Conf;

public class AsyncResource: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    Console.WriteLine("released");
  end DisposeAsync;
end AsyncResource;
