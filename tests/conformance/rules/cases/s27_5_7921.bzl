//! case: s27_5_7921
//! doc: spec
//! section: 27.5
//! title: a generic class's methods returning lambdas over its T = string parameter yield the
//!   text
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 14.1, 28.9
//! expect: run
//! stdout: 1200
//! notes: Fix pin for a generic-body closure leak/UAF (#8946): the corpus runner has no leak
//!   leg, so this asserts the program output; leak freedom is covered by the gtest rows.
module P;
public class Holder<T>
  public method Wrap(p: T): () -> T
    return lambda() => p;
  end Wrap;
  public method WrapB(p: T): () -> T
    return lambda() => do return p; end;
  end WrapB;
end Holder;

public class Program
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var h := new Holder<string>(); var f := h.Wrap("hi" + "x"); var g := h.WrapB("ab" + "c"); t := t + f().Length + g().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
