//! case: s2_2_0040
//! doc: spec
//! section: 2.2
//! title: an always-available name of the reference's arity is selected over an inherited nested type
//! rule: "Arity selects among the types rule 1 of §2.7 finds for an identifier: a
//!   reference resolves to the first, in that rule's order, whose arity its type-argument
//!   list fixes; where none has that arity, it resolves to the first of them and is
//!   Appendix C.1's arity error, never an alias, a member or a module prefix" (§2.2,
//!   v0.343). The reference `Task` fixes arity 0. Rule 1 of §2.7 finds the enclosing
//!   type's accessible members first, and `K` inherits the nested type `Task<T>` — but
//!   that candidate's arity is 1, so it does not end the search. The always-available
//!   names "stand in those rules where an imported bare name does" (§2.3, v0.343), and
//!   §2.3's table lists the Task family `Task`, `Task<T>`, `OwningTask<T>`: the
//!   non-generic `Task` has arity 0 and is the first candidate the reference's arity
//!   fixes, so `Take(t: Task)` names it and the program runs.
//! cites: 2.2, 2.3, 2.7
//! expect: run
//! stdout: reached
module Conf;

public class B
  public class Task<T>
    public method Get(): int => 1;
  end Task;
end B;

public class K: B
  public method Take(t: Task): int => 0;
  public static method Run(): int
    Console.WriteLine("reached");
    return 0;
  end Run;
end K;

public class Program
  public static method Main(): int => K.Run();
end Program;
