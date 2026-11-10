//! case: s28_3_0926
//! doc: spec
//! section: 28.3
//! title: under the report mode, a static store of one parameter into another's collection has no static form
//! rule: At each field-held, collection-held and comparer-held line the warning also
//!   states the classification §28.15 gives the store: **bounded**, where that
//!   section admits the store as written; **needs `scoped`**, where a member that
//!   is not a constructor keeps a plain parameter, naming the parameter; or **no
//!   static form**, naming the reason — the store relates two parameters, an
//!   `Object` parameter is kept by a member that is not a constructor, or the
//!   holder is static storage or is reached through a borrow. — §28.15: at any
//!   other location — a plain parameter's object — the store is a compile error
//!   unless it adds nothing
//! cites: 28.15, 28.11
//! expect: warn
//! diagnostic: no static form
//! flags: --report-trust-admissions
//! issue: 8420
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method N(): int => 1;
end Widget;

public class Program
  public static method Store(a: Array<Widget>, it: Widget): int
    a.Add(it);
    return a.Count;
  end Store;
end Program;
