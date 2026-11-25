//! case: s14_2_9306
//! doc: spec
//! section: 14.2
//! title: a nested bound over a sibling parameter rejects a type listing a different nested argument
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the rule above.
//! cites: 14.2, 14.1
//! expect: reject
//! diagnostic: constraint
//! notes: Harvested from the #9052 review (g02). Same program as s14_2_9305 with `string` for `int`: the bound is `IBag<Lst<string>>` and `LU` lists only `IBag<Lst<int>>`, which is a different constructed interface.
module Conf;
public interface IEntity<K>
  readonly property Id: K;
end IEntity;
public class User(id: int): IEntity<int>
  public readonly property Id: int := id;
end User;
public interface IBag<B>
end IBag;
public class Lst<Z>
end Lst;
public class LU: IBag<Lst<int>>
end LU;
public class Repo<K, T> where T: IBag<Lst<K>>
end Repo;
public class Program
  public static method Main(): int
    using var r := new Repo<string, LU>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
