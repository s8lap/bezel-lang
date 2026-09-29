# Proposal 0060 — The channel give-back protocol: TrySend, send cancellation, and cancellable/multi-source receive

| | |
|---|---|
| **Status** | under-review — draft (consolidation stub — collects the recorded design fragments the normative docs previously cited as "the give-back-protocol RFC" without a file existing) |
| **Tracking issue** | — (assign at first design pass) |
| **Target** | stdlib §6.9 (`Channel<T>`); spec §30.15; possibly §30.8 (cancellation composition). |
| **Written against** | spec v0.210 |
| **Currency note** | Re-verified clean at v0.306 (2026-09-17 proposals currency sweep): §30.15's give-back sentence (*"An item being handed to the waiter at the instant of disposal is returned to the channel's head (an internal give-back; items remain exactly-once)"*), stdlib §6.9's recorded `TrySend` deferral reason, and §22.3's unconditional `owned`-argument consume all still read as cited. No tier-1 respell, no tier-2 flag. Written-against pin unchanged. |
| **Origin** | The 2026-07-21 language-improvement sweep found spec §30.15 and stdlib §6.9 deferring four features "to the give-back-protocol RFC" — a design artifact that had never been written. This stub makes the referenced design real, consolidating the recorded fragments so the four features can graduate on **one** exactly-once-on-the-failure-edge argument instead of four parallel derivations. |

## 1. The one problem

Four deferred channel features share a single ownership question: **when an owned item is mid-handoff and the handoff fails or is abandoned, who holds the item?**

- **`TrySend`** — the decline path must hand the owned item *back* to the caller (stdlib §6.9's recorded reason).
- **Cancellation of a pending send** — same give-back, plus #1413's cancellation foundations.
- **Cancellable receive** (`Receive(token)`) and **multi-source select / `ReceiveAny`** — the v0.179 dispose-deregistration rule already defines *abandonment* (an item being handed to a disposed waiter returns to the channel head, exactly-once preserved — spec §30.15); a first-class primitive needs the same protocol on the cancel edge.

## 2. The recorded mechanism to generalize

v0.179's receive-abandonment (spec §30.15) is the shipped half: *"an item being handed to the waiter at the instant of disposal is returned to the channel's head (an internal give-back; items remain exactly-once)."* The protocol design generalizes this to a stated rule: **a failed or cancelled handoff returns the item to the side that owned it before the attempt** — sender-side failures leave (or place) the item with the sender (`TrySend` false-path: caller keeps ownership, spelled by the `owned` parameter *not* being consumed on the `false` return — a signature-level design question this proposal must answer, since §22.3's owned-argument consume is currently unconditional); receiver-side cancellations return the in-flight item to the channel head (the v0.179 rule, reused verbatim).

## 3. The design questions to answer (deliberately not answered in this stub)

1. **The conditional-consume signature.** `TrySend(owned item: T): bool` under §22.3 consumes unconditionally; the false path would need either (a) a conditional-consume marker (new kernel surface — interacts with A7's narrowed record and K3's enumeration), (b) an `out`-returned give-back (`TrySend(owned item: T, out returned: T?): bool` — clunky but zero new kernel rules), or (c) a `choice SendOutcome<T> { Sent; Declined(owned item: T) }` return (zero new kernel rules; the 0041 machinery carries the item back). Option (c) is the current lean: it spends no kernel budget.
2. **Select fairness and registration order** for `ReceiveAny`.
3. **Whether `TryReceive`'s existing shape constrains the select design** (it should compose, not compete).

## 4. Non-goals

Multi-consumer receiver cloning stays a separate question (§28.13 domain-locality — the one remaining reason of the MPMC deferral); capacity-zero rendezvous stays deferred on its own recorded reason.

## 5. Evidence log

*2026-07-21 (writer-seat sweep, worker-pipeline program):* all three of the program's top frictions were this proposal's surface — non-cancellable `Receive` silently defeating `TaskScope` supervision, no `TrySend`/cancellable send (producer-deadlock hazard), and the design nuance that a producer *outside* the nursery needs the send-side cancellation most. The N-channel round-robin replacement was reconstructed near-verbatim from stdlib §6.9's own example, confirming the sanctioned idiom is learnable but ceremonial. Strengthens the case for scheduling this design.

