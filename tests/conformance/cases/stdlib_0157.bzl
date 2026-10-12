//! case: stdlib_0157
//! doc: stdlib
//! section: 6.9
//! title: Channel<T>.Close() — receiver-initiated graceful close
//! rule: §30.15 (v0.334) / stdlib §6.9 (#1807) — "The receiving end may close
//!   first-class: `Close()` puts the channel in the same closed state —
//!   buffered items remain receivable, subsequent sends fault with
//!   ChannelClosedException (disposing the offered item, the rule) ... Close()
//!   is idempotent. Disposing the `Channel<T>` itself closes it immediately and
//!   disposes every undelivered class-typed item in reverse insertion order."
//!   The two postures are distinct: Close() leaves the undelivered items owned
//!   by the channel, so they are released exactly once by the channel's own
//!   disposal; a Send offered after Close is refused and its item disposed by
//!   the faulting send.

module M;
import System.Threading;

sealed class Item
  public V: int;
  public constructor(v: int) Item._n := Item._n + 1; V := v; end constructor;
  public static _n: int := 0;
  destructor Item._n := Item._n - 1; end destructor;
end Item;

public class C
  public static async method Main(): Task<int>
    using var ch := new Channel<Item>();
    using var tx := ch.NewSender();
    tx.Send(new Item(1));
    tx.Send(new Item(2));
    ch.Close();                          // closed, but NEITHER item disposed
    if not ch.IsClosed then return 1; end;
    if Item._n <> 2 then return 2; end;  // buffered items still the channel's
    ch.Close();                          // idempotent: a no-op when already closed
    var a := await ch.Receive();         // buffered items remain receivable
    var b := await ch.Receive();
    var sum := a.V * 10 + b.V;
    a.Dispose();
    b.Dispose();
    if sum <> 12 then return 3; end;
    if Item._n <> 0 then return 4; end;
    return 0;
  end Main;
end C;