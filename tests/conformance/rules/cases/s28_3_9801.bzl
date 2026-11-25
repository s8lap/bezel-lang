//! case: s28_3_9801
//! doc: spec
//! section: 28.3
//! title: A closure cannot store a captured owned parameter into an owning inline-array slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #9196 (pick 0ca67dfe2): the lambda captures the owned parameter item and stores it into
//!   a[0], an owning slot. The accepting twin is s28_3_9802, where the lambda takes its own owned
//!   parameter.
module Conf;

public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;

public class Repo
  public method Go(key: int, owned item: User)
    var a: [2 owned]User? := [null, null];
    var f := lambda(): int => do a[0] := item; return 0; end;
    _ := f();
  end Go;
end Repo;

public class Program
  public static method Main()
    using var r := new Repo();
    r.Go(1, new User(1));
    Console.WriteLine("done");
  end Main;
end Program;
