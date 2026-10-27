//! case: s14_2_0089
//! doc: spec
//! section: 14.2
//! title: a member a user interface inherits from a builtin owning interface is called at a constraint head
//! rule: Inside the generic body, T's surface includes IFoo's members, regardless
//!   of which conformance category T belongs to; the body cannot tell whether
//!   T was satisfied by a class, a record, an enum, an interface, a
//!   primitive, or another type parameter, and does not need to.
//!   The owning family's `Clear` disposes every element it holds (stdlib
//!   §4.2), so the call reaches that behaviour (§28.11).
//! cites: 14.2, 4.2, 28.11
//! expect: run
//! output: widget disposed
//!   after clear
//! issue: 7086
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public interface MyOwn: IOwningCollection<Widget>
end MyOwn;

public class Program
  public static method ClearIt<T>(target: T) where T: MyOwn, class
    target.Clear();
  end ClearIt;

  public static method Main(): int
    using var pool: OwningArray<Widget> := [new Widget()];
    Program.ClearIt(pool);
    Console.WriteLine("after clear");
    return 0;
  end Main;
end Program;
