//! case: s14_2_9305
//! doc: spec
//! section: 14.2
//! title: a nested constructed bound over a sibling parameter is substituted: Repo<int, LU> is admitted
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per the rule above.
//! cites: 14.2, 14.1
//! expect: run
//! output: ok
//! notes: Harvested from the #9052 review (g01). In `Repo<K, T> where T: IBag<Lst<K>>`, K := int gives the bound `IBag<Lst<int>>`, and `LU` lists exactly `IBag<Lst<int>>` (Interface conformance rule 1). Prints ok. Accepting twin of s14_2_9306.
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
    using var r := new Repo<int, LU>();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
