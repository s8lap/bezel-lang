//! case: s28_3_9803
//! doc: spec
//! section: 28.3
//! title: A closure nested in a closure cannot store the method's captured owned parameter into an owning slot
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 28.3.7, 28.9
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #9196 (pick 0ca67dfe2): the inner closure captures the owned parameter item of the
//!   enclosing method. Its accepting counterpart, with owned parameters at each lambda, is
//!   s28_3_9805.
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
    var f := lambda(): int => do
      var g := lambda(): int => do _items[key] := item; return 0; end;
      return g();
    end;
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
