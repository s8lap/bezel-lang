//! case: s14_2_9304
//! doc: spec
//! section: 14.2
//! title: a bound naming a later sibling parameter rejects a type that conforms only at another argument
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the rule above.
//! cites: 14.2, 14.1
//! expect: reject
//! diagnostic: constraint
//! notes: Harvested from the #9052 review (b02). Same program as s14_2_9303 with `string` for `int`: the bound is `IEntity<string>`, which `User` (listing only `IEntity<int>`) does not satisfy.
module Conf;
public interface IEntity<K>
  readonly property Id: K;
end IEntity;
public class User(id: int): IEntity<int>
  public readonly property Id: int := id;
end User;
public class Repo<T, K> where T: IEntity<K>
end Repo;
public class Program
  public static method Main(): int
    using var r := new Repo<User, string>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
