//! case: s28_8_1_4202
//! doc: spec
//! section: 28.8.1
//! title: a second bare take of a nullable IEnumerator field of a generic holder reads null
//! rule: A nullable location a bare take leaves holding `null` is an ordinary null field: its holder's cascade disposes nothing through it, and a later read reads `null`
//! cites: 28.8.1, 29.3, stdlib 4.12
//! expect: run
//! output: False
//!   True
//! issue: 7977
module Conf;

class G
  public static method It(): IEnumerator<int>
    yield return 5;
    yield return 6;
  end It;
end G;

class HG<T>
  public owned En: IEnumerator<T>? := null;

  public method Set(owned e: IEnumerator<T>)
    En := e;
  end Set;

  public method Take(): owned IEnumerator<T>?
    return move En;
  end Take;
end HG;

public class Program
  public static method Main()
    using var h := new HG<int>();
    h.Set(G.It());
    using var a := h.Take();
    using var b := h.Take();
    Console.WriteLine(a is null);
    Console.WriteLine(b is null);
  end Main;
end Program;
