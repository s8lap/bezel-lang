//! case: s28_3_9808
//! doc: spec
//! section: 28.3
//! title: A closure that takes its own owned parameter may assign it to a set(owned) property
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: accept
//! notes: Pins #9196 (pick 0ca67dfe2): the property target of the same shape: the lambda's own owned
//!   parameter reaches a set(owned) accessor.
module Conf;

public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;

public class Holder
  private owned _v: User? := null;
  public property Val: User
    get => _v!;
    set(owned) => _v := value;
  end Val;
end Holder;

public class Repo
  public method Go(key: int, owned item: User)
    using var h := new Holder();
    var f := lambda(owned u: User): int => do h.Val := u; return 0; end;
    _ := f(move item);
  end Go;
end Repo;
