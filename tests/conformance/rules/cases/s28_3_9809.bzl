//! case: s28_3_9809
//! doc: spec
//! section: 28.3
//! title: A closure may store a captured owned parameter into a non-owning dictionary slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: accept
//! notes: Pins #9196 (pick 0ca67dfe2): the captured owned parameter is read (borrowed) into a non-owning
//!   Dictionary slot: a read borrows in borrow state, and no consume occurs.
module Conf;

public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;

public class Repo
  public method Go(key: int, owned item: User)
    using var d := new Dictionary<int, User>();
    var f := lambda(): int => do d[key] := item; return 0; end;
    _ := f();
  end Go;
end Repo;
