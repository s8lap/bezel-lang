//! case: s28_8_1_0027
//! doc: spec
//! section: 28.8.1
//! title: after a destructor's bare take of a non-nullable field a read of the field is rejected
//! rule: at a non-nullable field the field holds no value for the rest of the
//!   body — a later read of it there through the field's own spelling is a
//!   read of a moved-from location ... and is rejected until the field is
//!   reassigned.
//! cites: 28.8.1, 28.4
//! expect: reject
//! diagnostic: moved-from
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
