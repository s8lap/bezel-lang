//! case: s28_8_1_4205
//! doc: spec
//! section: 28.8.1
//! title: a second bare take of a nullable IAsyncEnumerable field reads null
//! rule: A nullable location a bare take leaves holding `null` is an ordinary null field: its holder's cascade disposes nothing through it, and a later read reads `null`
//! cites: 28.8.1, 30.7, stdlib 4.12
//! expect: run
//! output: False
//!   True
//! issue: 7977
module Conf;

class H
  public owned Ae: IAsyncEnumerable<int>? := null;

  public async method Gen(): IAsyncEnumerable<int>
    yield return 1;
    yield return 2;
  end Gen;

  public method Fill()
    Ae := Gen();
  end Fill;

  public method Take(): owned IAsyncEnumerable<int>?
    return move Ae;
  end Take;
end H;

public class Program
  public static method Main()
    using var h := new H();
    h.Fill();
    using var a := h.Take();
    using var b := h.Take();
    Console.WriteLine(a is null);
    Console.WriteLine(b is null);
  end Main;
end Program;
