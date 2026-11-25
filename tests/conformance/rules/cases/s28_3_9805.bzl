//! case: s28_3_9805
//! doc: spec
//! section: 28.3
//! title: A nested closure that takes the value as its own owned parameter may store it into an owning slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: accept
//! notes: Pins #9196 (pick 0ca67dfe2): twin of s28_3_9804: each lambda owns its own owned parameter and
//!   passes it on with a marked move.
module Conf;

public class User(id: int)
  public readonly property Id: int := id;
  destructor
    Console.WriteLine($"close {Id}");
  end destructor;
end User;

public class Repo
  private owned _items: OwningDictionary<int, User> := new OwningDictionary<int, User>();
  public method Go(key: int, owned item: User)
    var f := lambda(owned u: User): int => do
      var g := lambda(owned w: User): int => do _items[key] := w; return 0; end;
      return g(move u);
    end;
    _ := f(move item);
  end Go;
end Repo;
