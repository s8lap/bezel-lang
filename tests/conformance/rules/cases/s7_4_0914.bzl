//! case: s7_4_0914
//! doc: spec
//! section: 7.4
//! title: indexing a ReadOnlyStringSpan? parameter that no test narrowed is rejected
//! rule: Member access, method calls, and indexing on a nullable value are compile errors unless the
//!   compiler has narrowed it to non-null
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  static method F(s: ReadOnlyStringSpan?): int
    Console.WriteLine(s[1]);
    return 0;
  end F;

  public static method Main()
    var r := F(null);
  end Main;
end Program;
