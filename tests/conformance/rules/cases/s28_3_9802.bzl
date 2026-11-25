//! case: s28_3_9802
//! doc: spec
//! section: 28.3
//! title: A closure that takes its own owned parameter may store it into an owning inline-array slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: accept
//! notes: Pins #9196 (pick 0ca67dfe2): twin of s28_3_9801: the value is the lambda's own owned parameter
//!   u, passed with a marked move from outside, which 28.3.7 states as the legal way to consume a
//!   value in a callback.
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
    var f := lambda(owned u: User): int => do a[0] := u; return 0; end;
    _ := f(move item);
  end Go;
end Repo;
