//! case: s14_2_9302
//! doc: spec
//! section: 14.2
//! title: a bound naming an earlier sibling parameter rejects a type that conforms only at another argument
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the rule above.
//! cites: 14.2, 14.1
//! expect: reject
//! diagnostic: constraint
//! notes: Harvested from the #9052 review (a02). Same program as s14_2_9301 with `string` for `int`: the bound becomes `IEntity<string>`, and `User` lists only `IEntity<int>`; `IEntity` declares no variance, so `IEntity<int>` is not `IEntity<string>` and `User` does not conform. The only ground is the `new Repo<string, User>()` instantiation.
module Conf;
public interface IEntity<K>
  readonly property Id: K;
end IEntity;
public class User(id: int): IEntity<int>
  public readonly property Id: int := id;
end User;
public class Repo<K, T> where T: IEntity<K>
end Repo;
public class Program
  public static method Main(): int
    using var r := new Repo<string, User>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
