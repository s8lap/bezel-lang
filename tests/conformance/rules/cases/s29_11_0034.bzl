//! case: s29_11_0034
//! doc: spec
//! section: 29.11
//! title: a same-unit iterator with no enumerator-owned yield stays silent
//! rule: A **same-unit iterator none of whose reachable yields is enumerator-owned**
//!   — every yield a re-borrow of a parameter-, field-, or caller-sourced element —
//!   must stay silent: the elements' owner is the caller's.
//! cites: 29.11, 29.2
//! expect: quiet
module Conf;

public class Doc
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine("doc closed");
  end destructor;
end Doc;

public class Index
  public method Visible(docs: Array<Doc>): IEnumerable<Doc>
    for var d in docs do
      yield return d;
    end for;
  end Visible;

  public method Head(docs: Array<Doc>)
    using var q := Visible(docs);
    var first := q.First();
    Console.WriteLine(first.Name);
  end Head;
end Index;
