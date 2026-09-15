//! case: s35_0002
//! doc: spec
//! section: 35
//! title: a nullable choice type does not satisfy the value constraint
//! rule: `where T: value` substitutions are non-nullable value types — records,
//!   enums, choices, primitives, inline arrays, the unit type. A nullable value
//!   type does not satisfy the constraint.
//! cites: 14.2, 35.9
//! expect: reject
//! diagnostic: value
module Conf;

public choice Status
  Ok,
  Failed(code: int);
end Status;

public class Program
  public static method Describe<T>(v: T): string where T: value
    return "value type";
  end Describe;

  public static method Main()
    Console.WriteLine(Describe<Status?>(Status.Failed(7)));
  end Main;
end Program;
