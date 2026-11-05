//! case: s7_4_0905
//! doc: spec
//! section: 7.4
//! title: a lambda testing a captured ReadOnlyStringSpan? may read members under its own test
//! rule: a null test in either spelling — `x <> null` or `x is not null` (narrowing the true branch)
//! cites: 7.4
//! expect: run
//! output: 3
module Conf;

public class Program
  public static method Main()
    var w := "hello";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 1, 3);
    var f := lambda(): int => do
      if s <> null then
        return s.Length;
      end if;
      return -1;
    end;
    Console.WriteLine(f());
  end Main;
end Program;
