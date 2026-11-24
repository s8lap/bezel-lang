//! case: s27_5_7919
//! doc: spec
//! section: 27.5
//! title: a generic method returning a block-bodied lambda over a temporary T argument yields
//!   its text
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 14.1, 28.9
//! expect: run
//! stdout: 1200
//! notes: Fix pin for a generic-body closure leak/UAF (#8946): the corpus runner has no leak
//!   leg, so this asserts the program output; leak freedom is covered by the gtest rows.
module P;

public class Program
  public static method Id<T>(p: T): () -> T
    return lambda() => do return p; end;
  end Id;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var f := Program.Id<string>("hi" + "x"); t := t + f().Length + f().Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine(Program.Run());
    return 0;
  end Main;
end Program;
