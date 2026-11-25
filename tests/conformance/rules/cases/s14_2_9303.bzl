//! case: s14_2_9303
//! doc: spec
//! section: 14.2
//! title: a bound naming a LATER sibling parameter is checked after substitution: Repo<User, int> is admitted
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the rule above.
//! cites: 14.2, 14.1
//! expect: run
//! output: ok
//! notes: Harvested from the #9052 review (b01). In `Repo<T, K> where T: IEntity<K>` the instantiation `Repo<User, int>` substitutes K := int, giving the bound `IEntity<int>`, which `User` satisfies by Interface conformance rule 1. Prints ok. Accepting twin of s14_2_9304.
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
    using var r := new Repo<User, int>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
