// bug: NB9
// title: an interface constraint whose type argument is another type parameter is not satisfied
// issue: new (#8754, #8728 adjacent)
// rests-on: spec §14.2: `User: IEntity<int>` satisfies `where T: IEntity<K>` at `K = int`
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 4
module P;
public interface IEntity<K>
  readonly property Id: K;
end IEntity;
public class Repo<K, T> where T: IEntity<K>
  public method IdOf(x: T): K => x.Id;
end Repo;
public class User(id: int): IEntity<int>
  public readonly property Id: int := id;
end User;
public class Program
  public static method Main()
    using var r := new Repo<int, User>();
    using var u := new User(4);
    Console.WriteLine(r.IdOf(u));
  end Main;
end Program;
