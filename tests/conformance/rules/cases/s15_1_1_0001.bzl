//! case: s15_1_1_0001
//! doc: spec
//! section: 15.1.1
//! title: move arr[i] on an owning inline array transfers ownership and requires restoration
//! rule: The `move arr[i]` form is admitted on `[N owned]T` slots; for
//!   non-nullable slots, restoration requires assigning a fresh owned T
//!   (§15.1.1).
//! cites: 15.1.1
//! expect: run
//! stdout: a
module Conf;

public class Connection
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;
end Connection;

public class Program
  public static method Main()
    var pool: [3 owned]Connection := [
      new Connection("a"), new Connection("b"), new Connection("c")
    ];
    var taken := move pool[0];
    pool[0] := new Connection("fresh");
    Console.WriteLine(taken.Name);
  end Main;
end Program;
