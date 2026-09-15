//! case: s30_10_0003
//! doc: spec
//! section: 30.10
//! title: the owning WhenAll overload aggregates OwningTask<T> results
//! rule: `Task.WhenAll<T>(owned tasks: OwningArray<OwningTask<T>>):
//!   OwningTask<OwningArray<T>> where T: class` — the caller transfers
//!   ownership of the array (and its tasks); the result is an owned array
//!   of owned elements.
//! cites: 30.10
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Loader
  public constructor()
  end constructor;

  public async method LoadAsync(): OwningTask<Widget>
    await Task.Delay(0);
    return new Widget();
  end LoadAsync;
end Loader;

public class Program
  public async method LoadAllAsync(): OwningTask<OwningArray<Widget>>
    using var loader := new Loader();
    var tasks := new OwningArray<OwningTask<Widget>>();
    tasks.Add(loader.LoadAsync());
    return await Task.WhenAll(move tasks);
  end LoadAllAsync;
end Program;
