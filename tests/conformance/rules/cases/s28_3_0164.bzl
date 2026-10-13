//! case: s28_3_0164
//! doc: spec
//! section: 28.3
//! title: under the report mode, moving a collection-held local into an owning array reports
//! rule: The site is the admitting site — the store, the call, the abandon or the
//!   `await` the clause admits — except for a collection element sourced in the
//!   body (a borrow of an owned local or `owned` parameter, a value that holds
//!   one, or an iterator, enumerator or lazy-query frame carrying one, or a
//!   borrow of such a frame, transitively), which is reported at the transfer of
//!   that binding by an obligation-moving kill that sets no flag the K4.1
//!   backstop can schedule (the consume table's rows K3.1–K3.4, K3.6, K3.7 and
//!   K3.12)
//! cites: 28.3, 28.11
//! expect: warn
//! diagnostic: trust
//! flags: --report-trust-admissions
//! issue: 4605
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Program
  public static method Run()
    using var pool := new OwningArray<Item>();
    var item := new Item();
    var coll := new Array<Item>();
    coll.Add(item);
    pool.Add(move item);
  end Run;
end Program;
