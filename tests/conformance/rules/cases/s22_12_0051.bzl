//! case: s22_12_0051
//! doc: spec
//! section: 22.12
//! title: the same static owned readonly field at Task of a class is admitted with rule 4's warning
//! rule: A `static readonly` (including `owned`) field or auto-property of
//!   **class type** is admitted with a **declaration-site warning** unless the
//!   class is in this rule's **warning-suppression set** — `OwningTask<T>`
//!   excepted, a compile error under (j)
//! cites: 22.12, 30.5
//! expect: accept
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public class Cache
  private static owned readonly _t: Task<Res> := Make();

  private static async method Make(): Task<Res>
    await Task.Delay(1);
    return new Res();
  end Make;
end Cache;
