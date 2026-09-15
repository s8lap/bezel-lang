//! case: s35_4_0001
//! doc: spec
//! section: 35.4
//! title: a value-like choice is an ordinary copyable value
//! rule: A value-like choice is an ordinary copyable value type: every payload is a
//!   value, a string, a runtime-managed type, or (transitively) a value-like
//!   record or choice.
//! expect: run
//! stdout: b active
module Conf;

public choice Status
  Active,
  Inactive;
end Status;

public class Program
  public static method Main()
    var a := Status.Active;
    var b := a;               // copy
    a := Status.Inactive;     // reassigning `a` does not affect `b`
    case b of
      Active   => Console.WriteLine("b active"),
      Inactive => Console.WriteLine("b inactive")
    end case;
  end Main;
end Program;
