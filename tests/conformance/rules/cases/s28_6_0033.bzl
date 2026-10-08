//! case: s28_6_0033
//! doc: spec
//! section: 28.6
//! title: this.DisposeAsync() in a CloseAsync-style method is rejected
//! rule: The borrow receivers: a borrowed local or parameter — including
//!   `this`: an object may not dispose itself out from under its owner ...
//!   no other `this`-rooted disposal call is, in that body or elsewhere.
//! cites: 28.6, 28.12, C.3
//! expect: reject
//! diagnostic: dispose itself
//! issue: 5660
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

  public async method CloseAsync(): Task
    await this.DisposeAsync();
  end CloseAsync;
end Resource;
