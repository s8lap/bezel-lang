//! case: s35_0001
//! doc: spec
//! section: 35
//! title: a choice type satisfies the value constraint
//! rule: `where T: value` substitutions are non-nullable value types: records,
//!   enums, choices, primitives, inline arrays, the unit type.
//! cites: 14.2, 35.4
//! expect: accept
module Conf;

public choice Status
  Ok,
  Failed(code: int);
end Status;

public class Program
  public static method Describe<T>(v: T): string where T: value
    return "value type";
  end Describe;

  public static method Use()
    var d := Describe<Status>(Status.Failed(7));
    Console.WriteLine(d);
  end Use;
end Program;
