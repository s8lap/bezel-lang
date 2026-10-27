//! case: s7_4_0113
//! doc: spec
//! section: 7.4
//! title: a member call on a nullable call result is rejected
//! rule: Member access, method calls, and indexing on a nullable value are
//!   compile errors unless the compiler has narrowed it to non-null. A call
//!   result is an expression, not a narrowing carrier, so it is read at its
//!   declared type.
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Make(): string? => "x";

  public static method Main(): int
    var t: string := Program.Make().Trim();
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
