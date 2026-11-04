//! case: s30_5_0026
//! doc: spec
//! section: 30.5
//! title: Task<T> over a borrow-bearing record is rejected at a parameter declaration
//! rule: A **value type that holds borrows** … is likewise admitted by **no
//!   row**: declaring `Task<T>` over such a T is a compile error at the
//!   declaration. … The rejection lands at the site that forms the type: a
//!   field, parameter, local or return type written `Task<Pair>`
//! cites: 30.5, 9.7, 3.6
//! expect: reject
//! diagnostic: holds borrows
//! notes: accepting twin s30_5_0027 — the one edit is the record type becoming
//!   CancellationToken, the row's certified exception.
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public record Pair(W: Widget);

public class Program
  public method Peek(t: Task<Pair>): bool
    return t.IsCompleted;
  end Peek;
end Program;
