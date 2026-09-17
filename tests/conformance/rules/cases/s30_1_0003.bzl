//! case: s30_1_0003
//! doc: spec
//! section: 30.1
//! title: an async method may return OwningTask<T> for a class-typed T
//! rule: `OwningTask<T>` for some class type T — a result whose ownership
//!   transfers to the awaiter (§30.5).
//! cites: 30.1, 30.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  public async method CreateAsync(): OwningTask<Widget>
    await Task.Delay(0);
    return new Widget();
  end CreateAsync;
end Factory;
