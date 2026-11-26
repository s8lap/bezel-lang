//! case: s3_4_3606
//! doc: spec
//! section: 3.4
//! title: a member may bear a reserved type name and is reached by qualification at a chain head
//! rule: A **member** may bear such a name — a field, a property, a method, an enum constant, a record's
//!   positional component: … it is reached bare as a value read or a callee, and at a chain head by
//!   qualification (`this.String.Length`).
//! cites: 3.4, 2.7
//! expect: run
//! stdout: 2
//! issue: 8115
//! notes: Twin of s3_4_3601: a member, not a binding, takes the name.
module Conf;

public class C
  public String: string := "ab";
  public constructor() end constructor;
  public method N(): int => this.String.Length;
end C;

public class Program
  public static method Main()
    using var c := new C();
    Console.WriteLine(c.N());
  end Main;
end Program;
