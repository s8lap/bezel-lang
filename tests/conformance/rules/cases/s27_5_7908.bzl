//! case: s27_5_7908
//! doc: spec
//! section: 27.5
//! title: a string parameter captured inside a nested block and read through the cell yields
//!   the text length
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! stdout: 3080
//! notes: Fix pin for a closure-cell leak: the corpus runner has no leak leg, so this asserts
//!   the program output; leak freedom is covered by the gtest rows.
module P;
public class Program
  public static method S(f: () -> string): string => f();
  public static method M(p: string): int
    if p.Length > 1 then
      var s := S(lambda() => p + "!");
      return s.Length + p.Length;
    end if;
    return 0;
  end M;
  public static method Main(): int
    var t := 0;
    for var j in 0..<300 do t := t + Program.M("ab" + j.ToString()); end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
