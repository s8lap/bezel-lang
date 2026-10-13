//! case: s3_6_0015
//! doc: spec
//! section: 3.6
//! title: the Id<Pair?> result used within its source's scope is admitted
//! rule: A rule that reads one of these properties off a declared, component or
//!   substituted type — §9.7's borrow-lifetime bullets, §28.3's
//!   bare-type-parameter clauses, §30.14's rules — reads `V?` as it reads
//!   `V`, whether the nullable form is written or manufactured by
//!   substitution (`T?` at `T = Pair`, or `T` at `T = Pair?`)
//! cites: 3.6, 9.7, 28.3
//! expect: run
//! stdout: 1
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public record Pair(It: Item);

public class Gen
  public static method Id<T>(a: T): T => a;
end Gen;

public class Program
  public static method Main()
    using var it := new Item();
    var p: Pair? := new Pair(it);
    var q := Gen.Id<Pair?>(p);
    Console.WriteLine(q!.It.Size());
  end Main;
end Program;
