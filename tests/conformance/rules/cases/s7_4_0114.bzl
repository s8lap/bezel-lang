//! case: s7_4_0114
//! doc: spec
//! section: 7.4
//! title: a member call on a nullable index result is rejected
//! rule: Member access, method calls, and indexing on a nullable value are
//!   compile errors unless the compiler has narrowed it to non-null. An index
//!   expression is an expression, not a narrowing carrier, so it is read at its
//!   declared type.
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Main(): int
    var a: Array<string?> := new Array<string?>();
    var t := a[0].Trim();
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
