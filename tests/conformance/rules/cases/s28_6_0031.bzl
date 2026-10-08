//! case: s28_6_0031
//! doc: spec
//! section: 28.6
//! title: a direct DisposeAsync() through a borrowed parameter is rejected
//! rule: `Dispose()` or a direct `DisposeAsync()` call through a **borrow** is
//!   a compile error. The borrow receivers: a borrowed local or parameter.
//! cites: 28.6, 28.12, C.3
//! expect: reject
//! diagnostic: borrow
//! issue: 5660
module Conf;

public class Program
  public static async method Close(d: IAsyncDisposable): Task
    await d.DisposeAsync();
  end Close;
end Program;
