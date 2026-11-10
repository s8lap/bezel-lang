//! case: s28_8_1_4206
//! doc: spec
//! section: 28.8.1
//! title: a second bare take of a nullable IOwningAsyncEnumerable field reads null
//! rule: A nullable location a bare take leaves holding `null` is an ordinary null field: its holder's cascade disposes nothing through it, and a later read reads `null`
//! cites: 28.8.1, 30.7, stdlib 4.13
//! expect: run
//! output: False
//!   True
//! issue: 7977
module Conf;

class R
  public constructor() end constructor;
end R;

class H
  public owned Ae: IOwningAsyncEnumerable<R>? := null;

  public async method Gen(): IOwningAsyncEnumerable<R>
    yield return new R();
  end Gen;

  public method Fill()
    Ae := Gen();
  end Fill;

  public method Take(): owned IOwningAsyncEnumerable<R>?
    return move Ae;
  end Take;
end H;

public class Program
  public static async method Main(): Task
    using var h := new H();
    h.Fill();
    await using var a := h.Take();
    await using var b := h.Take();
    Console.WriteLine(a is null);
    Console.WriteLine(b is null);
  end Main;
end Program;
