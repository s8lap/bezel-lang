# Probe results — bezelc 0.0.0-f6863e62 — 2026-10-28

Modes: report = --checked-disposal --report-trust-admissions; checked = --checked-disposal; unchecked = --unchecked-disposal. 'trust lines' counts 'trust admission recorded' warnings. Exit 70 = uncaught DisposedAccessException (r35's convention).

## k1a_owner_backpointer_cascade
> Kind 1: a child holds a back-pointer to the owner that holds it in an `owned` field. The owner's destructor body runs first (§28.4 step 1), then the owned fields cascade (step 2), so the child's destructor reads the owner AFTER the owner's body ran. EXPECT (spec): compiles; report names the store; run prints "owner body" then "child sees Root" (the owner's flag records that disposal began, checked mode faults from completion — measure).
- **report**: compile=accept-with-warning; trust lines=1
      k1a_owner_backpointer_cascade.bzl:18:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (n
  run exit=70; output: made|owner body|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|owner body|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: made|owner body|child sees Root|

## k1b_child_moved_out_then_owner_dies
> Kind 1, the subtlety: the child is moved OUT of its owner (a slot kill by move, §28.8.1), then the owner is disposed, then the child's back-pointer is read. The owner-outlives-owned proof does not cover a moved-out child. EXPECT (spec): compiles; checked mode raises DisposedAccessException at `c.OwnerName()`.
- **report**: compile=accept-with-warning; trust lines=1
      k1b_child_moved_out_then_owner_dies.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a pl
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## k1c_owning_collection_child_popped
> Kind 1 over an owning collection: children in an `owned OwningArray<Child>` each hold `this`; one is popped out (ownership leaves the owner), the owner is disposed, the popped child reads its back-pointer. EXPECT (spec): compiles; DisposedAccessException in checked mode at the second read; the un-popped child is disposed in the owner's cascade.
- **report**: compile=accept-with-warning; trust lines=1
      k1c_owning_collection_child_popped.bzl:17:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a pla
  run exit=70; output: Root|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Root|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: Root||

## k1d_grandchild_root_pointer
> Kind 1, a non-immediate ancestor: the root is passed down two constructors; the leaf holds the root. The leaf's store is a field-held admission (one report line); Mid only forwards. Structurally safe while every link is an owned field. EXPECT: compiles; prints Root; one report line.
- **report**: compile=accept-with-warning; trust lines=1
      k1d_grandchild_root_pointer.bzl:23:14: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non
  run exit=0; output: Root|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: Root|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: Root|

## k1e_scoped_this_backpointer
> Kind 1 spelled with today's static form: `new Child(scoped this)` under §28.14's `this` arm (form 3: the result is assigned to an owned field of this). EXPECT (spec): compiles; no report line (a scoped construction records no trust clause).
- **report**: compile=REJECT; trust lines=0
- **checked**: compile=REJECT; trust lines=0
- **unchecked**: compile=REJECT; trust lines=0

## k2a_registry_collection_held
> Kind 2: a long-lived registry holds borrows of items it does not own (collection-held clause). An item is disposed while registered; the registry's later sweep touches it. EXPECT (spec): compiles; one report line at `_items.Add(item)`; DisposedAccessException in checked mode at the sweep.
- **report**: compile=accept; trust lines=0
  run exit=70; output: a|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: a|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=2; output: a||

## k2b_weak_registry_today
> Kind 2, today's safe spelling: items become `Shared<Item>`, the registry keeps `Weak<Item>` observers and reads them through `TryUpgrade()`, which answers null for a dead item. EXPECT (spec): compiles; no report line; prints "a" then "gone"; never faults in either build mode.
- **report**: compile=REJECT; trust lines=0
      k2b_weak_registry_today.bzl:18:7: warning: `using var` `s` is vacuous — the RHS is a value-typed/runtime-managed value with no disposal obligation (§28.9); use `const 
- **checked**: compile=REJECT; trust lines=0
      k2b_weak_registry_today.bzl:18:7: warning: `using var` `s` is vacuous — the RHS is a value-typed/runtime-managed value with no disposal obligation (§28.9); use `const 
- **unchecked**: compile=REJECT; trust lines=0
      k2b_weak_registry_today.bzl:18:7: warning: `using var` `s` is vacuous — the RHS is a value-typed/runtime-managed value with no disposal obligation (§28.9); use `const 

## k3a_helper_source_disposed_early
> Kind 3: a short-lived helper built over a local, used and disposed in the same scope; the source is disposed early while the helper lives. EXPECT (spec): compiles (the holder is invisible to K5/0205 across the call); one report line at the store; DisposedAccessException in checked mode; undefined behaviour unchecked. Derived retention (0132) would make `c.Dispose()` precede a dominated holder use: a compile error.
- **report**: compile=accept-with-warning; trust lines=1
      k3a_helper_source_disposed_early.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## k3b_helper_source_transferred
> Kind 3, the silent kill: the source is MOVED into a longer-lived owner (no flag set) while the helper still holds it, then the helper reads it — admitted today (the holder is not a local borrow, K5 cannot see it). Here the new owner disposes it before the read. EXPECT (spec): compiles; DisposedAccessException in checked mode. Under derived retention the `move c` is K5.1-frozen.
- **report**: compile=accept-with-warning; trust lines=1
      k3b_helper_source_transferred.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (n
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## k3c_helper_scoped_static
> Kind 3 spelled statically with `scoped`: the source is frozen while the holder lives, so the early `c.Dispose()` is REJECTED at compile time (§28.14: the freeze covers explicit disposal). EXPECT (spec): compile error at `c.Dispose()`; no report line.
- **report**: compile=REJECT; trust lines=0
- **checked**: compile=REJECT; trust lines=0
- **unchecked**: compile=REJECT; trust lines=0

## k3d_factory_forwarding_virality
> Virality: the holder is built by a factory that forwards its plain parameter (`Open(c)` -> `new Account(c)`), and the factory is called through a second forwarding helper. A `kept`-style marker (or `scoped`) would be needed on Account's ctor, Open's and Wire's parameters: three signatures for one store. EXPECT (spec): compiles; one report line (the store); the early dispose faults in checked mode.
- **report**: compile=accept-with-warning; trust lines=1
      k3d_factory_forwarding_virality.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain 
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## k3e_fresh_temporary_into_storing_ctor
> Kind 3 at its narrowest: an unbound temporary passed to a storing constructor (FCG §4.3 corollary 5; DECLINED #5360). The temporary dies at the statement's end (K8.1); the holder dangles. EXPECT (spec): compiles; "customer disposed" printed before the read; DisposedAccessException in checked mode at `a.OwnerName()`. (The review's e06 found r35 does not dispose the temporary, #6775 — measure.)
- **report**: compile=accept-with-warning; trust lines=1
      k3e_fresh_temporary_into_storing_ctor.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a 
  run exit=0; output: tmp|customer disposed|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: tmp|customer disposed|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: tmp|customer disposed|

## k4a_interface_dispatch_store
> Kind 4: the store is behind interface dispatch; the caller sees only `ISink.Keep(x: Item)`. No body-derived fact can reach the caller through the interface (the implementation may be in another module). EXPECT (spec): compiles; one report line at the implementation's store; DisposedAccessException in checked mode at `Show()`.
- **report**: compile=accept-with-warning; trust lines=1
      k4a_interface_dispatch_store.bzl:18:40: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (no
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## k4b_function_value_store
> Kind 4: the storing callee is reached through a function value (`var f := k.Keep;` then `f(it)`): the caller's view is a function type that announces no retention. EXPECT (spec): compiles; one report line at the store; DisposedAccessException in checked mode.
- **report**: compile=accept-with-warning; trust lines=1
      k4b_function_value_store.bzl:14:40: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`o
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |
