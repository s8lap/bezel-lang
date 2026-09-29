//! case: s30_5_0011
//! doc: spec
//! section: 30.5
//! title: Task<T> over a move-only value T is a compile error at the declaration
//! rule: A **move-only value T** ... is admitted by **no row**: declaring
//!   `Task<T>` or `OwningTask<T>` over such a T is a **compile error at the
//!   declaration**.
//! cites: 30.5, 15.3
//! expect: reject
//! diagnostic: move-only
//! issue: 2255
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Packet
  Loaded(owned data: Widget),
  Empty;
end Packet;

public class Program
  public async method LoadAsync(): Task<Packet>
    await Task.Delay(0);
    return Packet.Empty;
  end LoadAsync;
end Program;
