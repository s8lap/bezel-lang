//! case: s28_8_1_4204
//! doc: spec
//! section: 28.8.1
//! title: a second bare take of a nullable IOwningEnumerator field reads null
//! rule: A nullable location a bare take leaves holding `null` is an ordinary null field: its holder's cascade disposes nothing through it, and a later read reads `null`
//! cites: 28.8.1, 29.3, stdlib 4.13
//! expect: run
//! output: False
//!   True
//! issue: 7977
module Conf;

class R
  public constructor() end constructor;
end R;

class G
  public static method It(): IOwningEnumerator<R>
    yield return new R();
  end It;
end G;

class H
  public owned En: IOwningEnumerator<R>? := null;

  public method Fill()
    En := G.It();
  end Fill;

  public method Take(): owned IOwningEnumerator<R>?
    return move En;
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
