//! case: s7_4_0910
//! doc: spec
//! section: 7.4
//! title: a ReadOnlyStringSpan? local declared with a non-nullable initializer is narrowed at its
//!   declaration
//! rule: from a declaration whose initializer is such an `expr` — `var x: T? := expr;` or `const x: T? :=
//!   expr;`
//! cites: 7.4
//! expect: run
//! output: e
module Conf;

public class Program
  public static method Main()
    var w := "hello";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 0, 5);
    Console.WriteLine(s[1]);
  end Main;
end Program;
