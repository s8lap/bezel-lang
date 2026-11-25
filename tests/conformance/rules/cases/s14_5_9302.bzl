//! case: s14_5_9302
//! doc: spec
//! section: 14.5
//! title: inferred type arguments are checked against a sibling-parameter bound: IdOf(u, "s") is rejected
//! rule: constraint satisfaction is checked after inference completes, on the full substitution, exactly as for a written type-argument list.
//! cites: 14.5, 14.2
//! expect: reject
//! diagnostic: constraint
//! notes: Harvested from the #9052 review (j04). Same program as s14_5_9301 with `"s"` for `7`: inference gives K := string and T := User, the bound on the full substitution is `IEntity<string>`, and `User` lists only `IEntity<int>`. The unsatisfied constraint is a compile error at the call, not an inference failure.
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
    Console.WriteLine(H.IdOf(u, "s"));
    return 0;
  end Main;
end Program;
