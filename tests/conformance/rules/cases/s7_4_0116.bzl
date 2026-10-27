//! case: s7_4_0116
//! doc: spec
//! section: 7.4
//! title: a member call on an unnarrowed nullable readonly field is rejected
//! rule: Member access, method calls, and indexing on a nullable value are
//!   compile errors unless the compiler has narrowed it to non-null. A nullable
//!   `readonly` field participates in flow narrowing — it is a narrowing carrier
//!   — but only once a narrowing form has established the fact.
//! cites: 7.4
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  private readonly _f: string?;

  public constructor() end constructor;

  public static method Main(): int
    var c := new Program();
    var t: string := c._f.Trim();
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
