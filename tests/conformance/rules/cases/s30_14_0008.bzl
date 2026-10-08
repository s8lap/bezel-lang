//! case: s30_14_0008
//! doc: spec
//! section: 30.14
//! title: the same sealed class with a string field and no compiler-known base is sendable
//! rule: A **class** type is sendable iff it is transfer-closed: the class is
//!   `sealed`, and every instance field, including inherited fields, is a
//!   sendable value type; `string`- or `Type`-typed; or an `owned` field
//!   whose declared type is a sendable class type.
//! cites: 30.14
//! expect: accept
module Conf;

import System.Threading;

public sealed class Fault
  public Detail: string := "disk";
end Fault;

public class Program
  public static method Handle(owned f: Fault)
  end Handle;

  public method Run()
    using var t := Domain.Spawn<Fault>(Program.Handle, new Fault());
  end Run;
end Program;
