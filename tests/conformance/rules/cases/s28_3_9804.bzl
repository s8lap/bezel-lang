//! case: s28_3_9804
//! doc: spec
//! section: 28.3
//! title: A nested closure cannot store the enclosing lambda's owned parameter into an owning slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #9196 (pick 0ca67dfe2): u is f's own owned parameter, but g captures it, so inside g it is
//!   a captured owned binding. Twin: s28_3_9805, where g takes u as its own owned parameter.
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
      var g := lambda(): int => do _items[key] := u; return 0; end;
      return g();
    end;
    _ := f(move item);
  end Go;
end Repo;

public class Program
  public static method Main()
    using var r := new Repo();
    r.Go(1, new User(1));
    Console.WriteLine("done");
  end Main;
end Program;
