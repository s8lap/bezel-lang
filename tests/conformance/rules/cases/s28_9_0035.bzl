//! case: s28_9_0035
//! doc: spec
//! section: 28.9
//! title: a class with an owning-choice field is not trivial-dispose and its undischarged local warns
//! rule: A class whose disposal **provably runs no user code and releases only
//!   memory** — it has no destructor, no `owned` fields of non-trivial-dispose
//!   type, no fields of move-only value type (§15.3 — an owning inline array,
//!   ... or an owning choice, nullable or not, §35.4 ...) ... — is a
//!   **trivial-dispose class**
//! cites: 28.9, 28.3, 15.3
//! expect: warn
//! diagnostic: must use
//! issue: 5822
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public choice Slot
  Held(owned r: Res),
  Empty;
end Slot;

public class Host
  private _slot: Slot := Slot.Held(new Res());
  public property Count: int => 1;
end Host;

public class Program
  public static method Size(): int
    var h := new Host();
    return h.Count;
  end Size;
end Program;
