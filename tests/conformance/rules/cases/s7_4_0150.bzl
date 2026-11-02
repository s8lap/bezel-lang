//! case: s7_4_0150
//! doc: spec
//! section: 7.4
//! title: a narrowed nullable `JsonOptions?` carrier's component read yields the component
//! rule: Member access, method calls, and indexing on a nullable value are compile errors unless the compiler has narrowed it to non-null:
//! cites: 7.4, 2.15
//! expect: run
//! output: 4
//! issue: 7993
module Conf;
import System.Serialization;

public class Program
  public static method Main(): int
    var o: JsonOptions? := new JsonOptions(true, 4, false, true, 9);
    if o <> null then
      Console.WriteLine(o.IndentWidth);
    end if;
    return 0;
  end Main;
end Program;
