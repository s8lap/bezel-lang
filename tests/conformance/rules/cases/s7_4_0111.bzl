//! case: s7_4_0111
//! doc: spec
//! section: 7.4
//! title: a method call on an unnarrowed nullable parameter is rejected
//! rule: Member access, method calls, and indexing on a nullable value are
//!   compile errors unless the compiler has narrowed it to non-null. A
//!   parameter is a narrowing carrier — "the compiler narrows local variables,
//!   parameters other than `ref` parameters, and `readonly` fields" — but only
//!   once one of the narrowing forms has established the fact; a parameter that
//!   was never narrowed is read at its declared type.
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method P(s: string?): int
    var t: string := s.Trim();
    return t.Length;
  end P;
end Program;
