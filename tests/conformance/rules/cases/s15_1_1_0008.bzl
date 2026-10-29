//! case: s15_1_1_0008
//! doc: spec
//! section: 15.1.1
//! title: a static owning-inline-array field disposes its slots in reverse slot order at program exit
//! rule: A fourth ending is **program exit**, for a static field of
//!   owning-inline-array type: the exit pass disposes each non-null slot of
//!   each such field, in this section's reverse slot order, taking the fields
//!   themselves in reverse order of static initialization.
//! cites: 15.1.1, 22.13, 22.12
//! expect: run
//! warns: yes
//! output: first
//!   dispose second
//!   dispose first
//! issue: 4388
module Conf;

public class Connection
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine($"dispose {Name}");
  end destructor;
end Connection;

public class Registry
  public static readonly Pool: [2 owned]Connection := [new Connection("first"), new Connection("second")];
end Registry;

public class Program
  public static method Main()
    Console.WriteLine(Registry.Pool[0].Name);
  end Main;
end Program;
