//! case: s7_4_0909
//! doc: spec
//! section: 7.4
//! title: a narrowed ReadOnlyStringSpan? receiver reaches the char overload of Contains
//! rule: a null test in either spelling — `x <> null` or `x is not null` (narrowing the true branch)
//! cites: 7.4
//! expect: run
//! output: True
module Conf;

public class Program
  public static method Main()
    var w := "hello";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 1, 2);
    if s <> null then
      Console.WriteLine(s.Contains('l'));
    end if;
  end Main;
end Program;
