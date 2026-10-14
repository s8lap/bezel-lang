//! case: s7_4_0048
//! doc: spec
//! section: 7.4
//! title: a narrowing survives a loop back-edge when the loop reassigns from non-nullable
//! rule: after `x := expr;` where `expr`'s static type is non-nullable — or `expr` is itself narrowed non-null at that point — `x` is narrowed until invalidated (v0.338 §7.4 line 903)
//! cites: 7.4
//! expect: run
//! output: 2
//!   2
module Conf;

public class Program
  public static method Probe(s: string?)
    if s <> null then
      var i := 0;
      while i < 2 do
        Console.WriteLine(s.Length);
        s := "cd";
        i := i + 1;
      end while;
    end if;
  end Probe;

  public static method Main()
    Probe("ab");
  end Main;
end Program;
