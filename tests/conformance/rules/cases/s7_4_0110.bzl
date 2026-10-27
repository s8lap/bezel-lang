//! case: s7_4_0110
//! doc: spec
//! section: 7.4
//! title: a method call on an unnarrowed nullable local is rejected
//! rule: Member access, method calls, and indexing on a nullable value are
//!   compile errors unless the compiler has narrowed it to non-null.
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main(): int
    var s: string? := null;
    var t: string := s.Trim();
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
