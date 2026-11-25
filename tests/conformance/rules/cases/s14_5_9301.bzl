//! case: s14_5_9301
//! doc: spec
//! section: 14.5
//! title: inferred type arguments are checked against a sibling-parameter bound: IdOf(u, 7) is admitted
//! rule: constraint satisfaction is checked after inference completes, on the full substitution, exactly as for a written type-argument list.
//! cites: 14.5, 14.2
//! expect: run
//! output: 1
//! notes: Harvested from the #9052 review (j03). `IdOf<K, T>(x: T, k: K) where T: IEntity<K>` called as `IdOf(u, 7)` infers K := int and T := User; the bound is checked on the full substitution, `IEntity<int>`, which `User` satisfies. The body returns 1. Accepting twin of s14_5_9302.
module Conf;
public interface IEntity<K>
  readonly property Id: K;
end IEntity;
public class User(id: int): IEntity<int>
  public readonly property Id: int := id;
end User;
public class H
  public static method IdOf<K, T>(x: T, k: K): int where T: IEntity<K> => 1;
end H;
public class Program
  public static method Main(): int
    using var u := new User(4);
    Console.WriteLine(H.IdOf(u, 7));
    return 0;
  end Main;
end Program;
