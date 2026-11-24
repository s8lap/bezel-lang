//! case: s27_5_7901
//! doc: spec
//! section: 27.5
//! title: a nested lambda over two string parameters returned and called 200 times yields the
//!   captured text
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 2800
//! notes: Fix pin for a closure-cell leak: the corpus runner has no leak leg, so this asserts
//!   the program output; leak freedom is covered by the gtest rows.
module P;
public class Program
  public static method Ident(p: string, q: string): () -> () -> string => lambda() => lambda() => p + q;
  public static method Run(): int
    var s := "hi" + "x";
    var t := 0;
    for var j in 0..<200 do
      var f := Program.Ident(s, s + "y");
      t := t + f()().Length + f()().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
