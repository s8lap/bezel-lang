//! case: s29_11_0033
//! doc: spec
//! section: 29.11
//! title: a terminal accessor over a concrete collection root stays silent
//! rule: Every other root stays **silent**, and this is the exhaustive list: a
//!   **concrete collection value** as the root — a `new` collection, or a
//!   collection-typed parameter, field, or local bound to one — must stay silent,
//!   the elements' owner being the collection, which outlives the accessor call.
//! cites: 29.11, 28.3
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
  public method Head(docs: Array<Doc>)
    var first := docs.First();
    Console.WriteLine(first.Name);
  end Head;
end Index;
