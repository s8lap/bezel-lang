//! case: s29_11_0035
//! doc: spec
//! section: 29.11
//! title: making the same iterator's yield a fresh producer draws the provenance warning
//! rule: A call to a class-element terminal accessor **must draw a warning** iff the
//!   receiver's statically-tracked provenance roots at a directly-called iterator
//!   method, compiled in the same compilation unit, at least one of whose reachable
//!   `yield return` sites classifies as an enumerator-owned yield — minting, a fresh
//!   producer among them.
//! cites: 29.11, 29.2
//! expect: warn
//! diagnostic: provenance
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
      yield return new Doc(d.Name);
    end for;
  end Visible;

  public method Head(docs: Array<Doc>)
    using var q := Visible(docs);
    var first := q.First();
    Console.WriteLine(first.Name);
  end Head;
end Index;
