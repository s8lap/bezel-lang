//! case: s28_12_0012
//! doc: spec
//! section: 28.12
//! title: await using var over a borrow is a pin that disposes and awaits nothing
//! rule: Over a borrow right-hand side (a call without an `owned` return, a
//!   field, an indexer, a borrow-state binding) the binding assumes no
//!   obligation, so scope exit disposes nothing and awaits nothing: the form
//!   is the same compile-time pin as under `using var`, with the same freeze
//!   on the source.
//! cites: 28.12, 28.10
//! expect: run
//! output: inner
//!   after
//!   sync cleanup
module Conf;

public class Widget: IAsyncDisposable
  public constructor()
  end constructor;

  public method Ping()
  end Ping;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("async cleanup");
  end DisposeAsync;

  destructor
    Console.WriteLine("sync cleanup");
  end destructor;
end Widget;

public class Holder
  private owned _w: Widget;

  public constructor()
    _w := new Widget();
  end constructor;

  public method Borrowed(): Widget => _w;
end Holder;

public class Program
  public static async method Main(): Task
    using var h := new Holder();
    do
      await using var view := h.Borrowed();
      view.Ping();
      Console.WriteLine("inner");
    end;
    Console.WriteLine("after");
  end Main;
end Program;
