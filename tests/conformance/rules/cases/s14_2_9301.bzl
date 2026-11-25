//! case: s14_2_9301
//! doc: spec
//! section: 14.2
//! title: a bound naming an EARLIER sibling parameter is checked after substitution: Repo<int, User> admitted
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the rule above.
//! cites: 14.2, 14.1
//! expect: run
//! output: ok
//! notes: Harvested from the #9052 review (a01). In `Repo<K, T> where T: IEntity<K>` the instantiation `Repo<int, User>` substitutes K := int, so the bound is `IEntity<int>`, and `User` lists `IEntity<int>` (Interface conformance rule 1), so it conforms. Prints ok. Accepting twin of s14_2_9302.
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
    using var r := new Repo<int, User>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
