//! case: s27_5_7904
//! doc: spec
//! section: 27.5
//! title: a nested lambda built inside a lambda that captures the method's string parameter
//!   reads the capture
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 3560
//! notes: Fix pin for a closure-cell leak: the corpus runner has no leak leg, so this asserts
//!   the program output; leak freedom is covered by the gtest rows.
module P;
public class Program
  public static method Ident(p: string, k: int): () -> () -> string
    var mk: (string) -> () -> string := lambda(q: string) => lambda() => q + p;
    var inner := mk(p);
    return lambda() => inner;
  end Ident;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s := "hi" + j.ToString();
      var f := Program.Ident(s, j);
      t := t + f()().Length + f()().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
