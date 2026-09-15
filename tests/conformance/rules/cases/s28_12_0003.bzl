//! case: s28_12_0003
//! doc: spec
//! section: 28.12
//! title: a class implementing IAsyncDisposable without a destructor draws a compiler warning
//! rule: The compiler emits a warning when a class implements
//!   IAsyncDisposable without a destructor -- "synchronous Dispose() will
//!   skip async cleanup."
//! cites: 28.12
//! expect: warn
//! diagnostic: skip async cleanup
module Conf;

import System.Threading;

public class Resource: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
  end DisposeAsync;
end Resource;
