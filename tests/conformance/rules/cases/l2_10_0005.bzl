//! case: l2_10_0005
//! doc: stdlib
//! section: 2.10
//! title: IAsyncDisposable is implemented explicitly with an async DisposeAsync
//! rule: IAsyncDisposable is implemented explicitly by classes that need
//!   asynchronous cleanup; implementations declare a method matching
//!   DisposeAsync(): Task and add async -- async never appears on the
//!   interface declaration itself. A class implementing IAsyncDisposable also
//!   declares a destructor for the synchronous fallback path.
//! cites: 28.12
//! expect: accept
module Conf;

public class AsyncResource: IAsyncDisposable
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("released synchronously");
  end destructor;

  public async method DisposeAsync(): Task
    Console.WriteLine("released");
  end DisposeAsync;
end AsyncResource;
