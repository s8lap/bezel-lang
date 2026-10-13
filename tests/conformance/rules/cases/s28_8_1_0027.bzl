//! case: s28_8_1_0027
//! doc: spec
//! section: 28.8.1
//! title: a destructor body's bare take of a non-nullable field is rejected as in any other body
//! rule: Inside a destructor body, the fields of the object being torn down are
//!   owned locations like any other: `move x.f with e` exchanges, `move x.f`
//!   at a nullable field stores `null`, and a non-nullable field is not taken
//!   bare — the compile error names, beside the two remedies above, the
//!   field's explicit `Dispose()` where the destructor's class declares the
//!   field.
//! cites: 28.8.1, 28.4
//! expect: reject
//! diagnostic: with
//! issue: 5923
module Conf;

public class Res
  public readonly property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Res;

public class Holder
  private owned _r: Res := new Res("a");

  destructor
    using var t := move _r;
    Console.WriteLine(_r.Name);
  end destructor;
end Holder;
