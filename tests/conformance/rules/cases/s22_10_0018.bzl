//! case: s22_10_0018
//! doc: spec
//! section: 22.10
//! title: an unreferenced scoped primary parameter captures no field and its call site is still admitted
//! rule: The freeze is keyed on the signature, not on the capture: a
//!   `scoped` parameter referenced in no body and no initializer captures no
//!   field, and the call site freezes its source all the same, harmlessly.
//! cites: 22.10, 28.14
//! expect: accept
//! issue: 4601
module Conf;

public class Source
  public constructor()
  end constructor;
end Source;

public class Tag(scoped src: Source, label: string)
  public method Name(): string => label;
end Tag;

public class Program
  public method Demo()
    using var s := new Source();
    using var t := new Tag(scoped s, "x");
    Console.WriteLine(t.Name());
  end Demo;
end Program;
