//! case: s7_4_0912
//! doc: spec
//! section: 7.4
//! title: a ReadOnlyStringSpan? narrowed by a test and then assigned null does not admit a member read
//! rule: the variable is reassigned from a nullable-typed expression
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main()
    var w := "hello";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 0, 5);
    if s <> null then
      s := null;
      Console.WriteLine(s.Length);
    end if;
  end Main;
end Program;
