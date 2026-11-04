//! case: s26_5_0065
//! doc: spec
//! section: 26.5
//! title: an instance method called through a constructed type name is rejected
//! rule: Through a type name (§2.7 rule 1) the chain resolves against the
//!   type's nested types, enum constants and static members, declared or
//!   inherited, and a call or read `T.M` that none of them supplies names no
//!   member and is the same compile error: an instance method of the name
//!   does not supply it, there being no receiver, and an extension does not,
//!   extensions having no static-invocation form (rule 4).
//! cites: 26.5, 2.7
//! expect: reject
//! diagnostic: Sort
//! issue: 7699
//! notes: #8065 asks whether §27.6's unbound instance-method references `T.M` conflict with this sentence. The verdict follows §26.5 as written.
module Conf;

public class Program
  public static method Order(a: Array<int>)
    Array<int>.Sort(a);
  end Order;
end Program;
