//! case: s7_4_1201
//! doc: spec
//! section: 7.4
//! title:a narrowed nullable span reassigned from a non-null value still admits a member read
//! rule:Member access, method calls, and indexing on a nullable value are compile errors unless the
//!   compiler has narrowed it to non-null
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Main()
    var w := "hello";
    var s: ReadOnlyStringSpan? := new ReadOnlyStringSpan(w, 0, 5);
    if s <> null then
      s := new ReadOnlyStringSpan(w, 1, 2);
      Console.WriteLine(s.Length);
    end if;
  end Main;
end Program;
