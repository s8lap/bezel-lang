# Probe results — bezelc 0.0.0-a30bb11d — 2026-11-10

Modes: report = --checked-disposal --report-trust-admissions; checked = --checked-disposal; unchecked = --unchecked-disposal. 'trust lines' counts 'trust admission recorded' warnings. Exit 70 = uncaught DisposedAccessException.

## a01_alias_across_await
> ASYNC baseline: a plain ALIAS of a plain parameter is live across an `await`; the caller awaits before the source dies. EXPECT (spec 30.1, borrowed parameters are held until settlement): compiles; no report line at this await; clean. Control for a02.
- **report**: compile=accept-with-warning; trust lines=1
      a01_alias_across_await.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must ke
  run exit=0; output: ann|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: ann|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: ann|

## a02_holder_across_await
> ASYNC, the question: a local instance that holds a borrow of a plain parameter is live across an `await` (the routine async helper). The frame already holds `c` itself until settlement, so the holder adds no source the frame does not carry. EXPECT today: compiles; one report line (Account store); clean when awaited. Under 0212: admitted — every source of a borrow formed in an async body is frame-resident or already carried by the frame (30.1).
- **report**: compile=accept-with-warning; trust lines=1
      a02_holder_across_await.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must k
  run exit=0; output: ann|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: ann|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: ann|

## a03_holder_across_await_source_killed
> ASYNC, the residual that stays: the caller disposes the source while the task is unsettled, then awaits. The holder inside the frame reads a disposed object. This is the frame-held clause of 30.1 (settlement), which 0212 does not retire — the same fault occurs with the plain alias of a01. EXPECT today: compiles; checked = DisposedAccessException. Under 0212: unchanged (frame-held clause; a task is not traced by K5, K6.4).
- **report**: compile=accept-with-warning; trust lines=1
      a03_holder_across_await_source_killed.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## a04_owning_task_result_carries
> ASYNC, a result that holds a borrow: an `async` method returns an `OwningTask<Account>` built over its plain parameter. The awaited result is an owned instance whose stored borrow is the caller's `c`. EXPECT today: compiles; checked = DisposedAccessException after `c.Dispose()`. Under 0212: the task carries `c` (K6), the awaited result carries the task's carried set, so the use after the dominating `c.Dispose()` is the 28.2 error.
- **report**: compile=accept-with-warning; trust lines=1
      a04_owning_task_result_carries.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## a05_holder_param_into_async
> ASYNC, a holder passed as a plain parameter: the frame holds a borrow of `acct`, and through it of `c`. EXPECT today: compiles; clean when awaited. Under 0212: the argument contributes its binding and the sources it carries to the task (K6), which is the frame-held clause unchanged; the awaited call is clean.
- **report**: compile=accept-with-warning; trust lines=1
      a05_holder_param_into_async.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller mu
  run exit=0; output: ann|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: ann|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: ann|

## a06_async_lambda_captures_holder
> ASYNC, a holder captured by an `async` lambda: the closure's set gains the holder's binding and the sources it carries; the task its invocation mints carries them. EXPECT today: compiles; clean when awaited. Under 0212: the same, with the capture counted (28.3 provenance clause 1).
- **report**: compile=accept-with-warning; trust lines=1
      a06_async_lambda_captures_holder.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the call
  run exit=0; output: ann|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: ann|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: ann|

## a07_owned_holder_into_async_frame
> ASYNC, an OWNED holder transferred into the frame: the frame owns the `Account`, whose stored borrow is still the caller's `c`. EXPECT today: compiles; checked = DisposedAccessException when the caller disposes `c` before settlement. Under 0212: the owned argument brings its set to the task (K6 + the owned-argument rule), which is the frame-held clause: the report names the abandon, the verdict is unchanged.
- **report**: compile=accept-with-warning; trust lines=1
      a07_owned_holder_into_async_frame.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the cal
      a07_owned_holder_into_async_frame.bzl:18:35: warning: owned parameter `a` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using va
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept-with-warning; trust lines=0
      a07_owned_holder_into_async_frame.bzl:18:35: warning: owned parameter `a` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using va
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept-with-warning; trust lines=0
      a07_owned_holder_into_async_frame.bzl:18:35: warning: owned parameter `a` is implicitly disposed by the frame at teardown (§28.3/§29.8/§30.1 must-use / auto-drop); `move` / `return` / `.Dispose()` it, or adopt it with `using va
  run exit=0; output: |

## a08_alias_across_await_source_killed
> ASYNC, the control for a03: a plain ALIAS of a plain parameter is live across an `await`, and the caller disposes the source while the task is unsettled. EXPECT (spec 30.1, the frame-held clause): compiles; checked = DisposedAccessException. The same residual as a03, with no holder in the frame.
- **report**: compile=accept-with-warning; trust lines=1
      a08_alias_across_await_source_killed.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the 
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## d01_dtor_body_uses_this
> DESTRUCTOR WINDOW, flag timing: the owner's own destructor body (a) reads an expression-bodied property of `this`, (b) calls an instance method of `this`, (c) hands `this` to a static helper that reads a property. MEASURE which of the three faults in checked mode: it tells whether the disposed flag is set before the destructor body runs, and whether a member entry on `this` inside the body is flag-tested.
- **report**: compile=accept; trust lines=0
  run exit=70; output: made|field: Root|property: Root|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|field: Root|property: Root|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: made|field: Root|property: Root|method: owner Root|helper: Root|

## d02_owner_disposes_child_in_body
> DESTRUCTOR WINDOW, the early-dispose idiom: the owner's destructor BODY disposes its back-pointing child explicitly (K9.3 admits disposing an owned field its class declares; the cascade's later call is the no-op). The child's destructor reads the owner while the owner's body is still running and every owned field is live. MEASURE: fault or clean, both modes.
- **report**: compile=accept-with-warning; trust lines=1
      d02_owner_disposes_child_in_body.bzl:20:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the call
  run exit=70; output: made|owner body begins|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|owner body begins|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: made|owner body begins|child sees Root|owner body ends|

## d03_child_reads_owner_field_in_cascade
> DESTRUCTOR WINDOW, k1a with a FIELD read: the child's destructor reads a public field of its owner (no accessor call) during the owner's cascade; the owner has no destructor body. MEASURE: fault in checked mode? output unchecked?
- **report**: compile=accept-with-warning; trust lines=1
      d03_child_reads_owner_field_in_cascade.bzl:14:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source th
  run exit=70; output: made|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: made|child sees Root|

## d04_child_reads_later_sibling_through_owner
> DESTRUCTOR WINDOW, the genuinely dangling read: the child (declared FIRST, so disposed LAST) reads, through its owner, a sibling declared after it, which the cascade (reverse declaration order, §28.4) has already disposed. MEASURE: checked = fault; unchecked = ? (this is the read that is unsound even if the owner's block is live).
- **report**: compile=accept-with-warning; trust lines=1
      d04_child_reads_later_sibling_through_owner.bzl:25:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose sour
  run exit=70; output: made|leaf disposed late|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|leaf disposed late|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=139; output: made|leaf disposed late|

## d05_trivial_child_backpointer
> DESTRUCTOR WINDOW, control: a back-pointing child with NO destructor (trivial-dispose). Nothing runs at its release, so nothing reads the owner in the cascade. EXPECT: clean in both modes.
- **report**: compile=accept-with-warning; trust lines=1
      d05_trivial_child_backpointer.bzl:19:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller 
  run exit=0; output: Root|owner body|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: Root|owner body|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: Root|owner body|

## d06_sibling_order
> DESTRUCTOR WINDOW, siblings: `_cache` borrows its sibling `_db`. Right order: the borrowing field is declared AFTER the field it borrows, so the cascade disposes it first and its destructor reads a live sibling. EXPECT: clean in both modes. (d07 is the wrong order.)
- **report**: compile=accept-with-warning; trust lines=1
      d06_sibling_order.bzl:16:12: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must keep al
  run exit=0; output: made|cache flushes to main|db closed|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: made|cache flushes to main|db closed|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: made|cache flushes to main|db closed|

## d07_sibling_wrong_order
> DESTRUCTOR WINDOW, siblings, wrong order: `_cache` is declared BEFORE the `_db` it borrows, so the cascade disposes `_db` first and the cache's destructor reads a disposed sibling. EXPECT today: compiles; checked = DisposedAccessException. Under 0212: error at the store (the declared-before test on fields).
- **report**: compile=accept-with-warning; trust lines=1
      d07_sibling_wrong_order.bzl:16:12: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must k
  run exit=70; output: made|db closed|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|db closed|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=139; output: made|db closed|

## d08_base_dtor_virtual_call
> DESTRUCTOR WINDOW, the base-class route: destructors run subclass-to-base (§28.4), so the BASE destructor body runs after the derived class's owned fields are disposed. It calls a virtual member the derived class overrides, which reads a derived owned field. MEASURE: checked / unchecked. (Not a stored-borrow read: recorded to show the window exists without any back-pointer.)
- **report**: compile=accept; trust lines=0
  run exit=70; output: made|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=139; output: made|

## d09_external_holder_dtor_reads_source
> DESTRUCTOR WINDOW, the external-source holder (flush-on-dispose): a holder whose destructor reads a source declared BEFORE it. Scope end disposes in reverse declaration order, so the holder's destructor reads a live source. EXPECT: clean in both modes. This is the idiom a blanket "no stored-borrow read in a destructor" rule would retire.
- **report**: compile=accept-with-warning; trust lines=1
      d09_external_holder_dtor_reads_source.bzl:16:14: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the
  run exit=0; output: made|out <- flush|sink closed|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: made|out <- flush|sink closed|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: made|out <- flush|sink closed|

## d10_external_holder_wrong_order
> DESTRUCTOR WINDOW, the external-source holder declared BEFORE its source (a later store): the source is disposed first at scope end and the holder's destructor reads it. EXPECT today: compiles (field-held trust); checked = DisposedAccessException. Under 0212: error at the store (the holder's release is a use, so the source must be declared before the holder).
- **report**: compile=accept-with-warning; trust lines=1
      d10_external_holder_wrong_order.bzl:14:48: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the calle
  run exit=70; output: made|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=139; output: made|

## d11_collection_child_reads_owner
> DESTRUCTOR WINDOW, an owning collection of back-pointing children (the tree with parent pointers): each node's destructor reads its parent during the parent's cascade. MEASURE: checked = fault at the first child; unchecked = ?
- **report**: compile=accept-with-warning; trust lines=1
      d11_collection_child_reads_owner.bzl:9:16: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the calle
  run exit=70; output: made|root is the root|Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: made|root is the root|Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: made|root is the root|b leaves root|a leaves root|

## d12_tree_no_dtor_reads
> DESTRUCTOR WINDOW, control: the same tree, with a destructor that does NOT read the parent. EXPECT: clean in both modes. (A rule keyed on "the child is not trivial-dispose" would reject this tree: OwningArray is never trivial-dispose.)
- **report**: compile=accept-with-warning; trust lines=1
      d12_tree_no_dtor_reads.bzl:9:16: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must kee
  run exit=0; output: root/a/b|root disposed|a disposed|b disposed|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: root/a/b|root disposed|a disposed|b disposed|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: root/a/b|root disposed|a disposed|b disposed|

## g01_generic_hide_owned_T_as_interface
> GENERIC ERASURE, the note's hole: a generic body converts `owned T` to an `owned` interface. Instantiated at a class that holds a borrow, the result's type announces nothing and the body is not re-checked. EXPECT today: compiles; one report line (Account's store); checked = DisposedAccessException; unchecked = undefined. Under 0212: `Hider.Hide<Account>` is rejected where it is named (the conversion is a requirement of the body's use of T, read per instantiation).
- **report**: compile=accept-with-warning; trust lines=1
      g01_generic_hide_owned_T_as_interface.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## g02_generic_hide_through_second_generic
> GENERIC ERASURE through a second generic: `Outer<T>` only forwards its `owned T` to `Hider.Hide<T>`. EXPECT today: compiles; faults as g01. Under 0212: the requirement reaches `Outer` through the substitution (spec 3.6 rule 2), so `Outer<Account>` is rejected where it is named.
- **report**: compile=accept-with-warning; trust lines=1
      g02_generic_hide_through_second_generic.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source t
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## g03_generic_type_stores_T_in_interface_field
> GENERIC ERASURE in a generic TYPE: the constructor stores its `owned T` in an `owned` field declared at an interface. EXPECT today: compiles; faults. Under 0212: a requirement of the constructed type, so `Erase<Account>` is rejected wherever it is named (as `Keep<Pair>` is today).
- **report**: compile=accept-with-warning; trust lines=1
      g03_generic_type_stores_T_in_interface_field.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose sou
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## g04_dispatched_generic_method_erases
> GENERIC ERASURE in a DISPATCHED generic method: an interface method with its own type parameter has no instantiation site for the bodies dispatch reaches (spec 14.2). The implementing body stores `owned T` at an interface type. EXPECT today: compiles; faults. Under 0212: an error at the implementing body unless the constraint set establishes that T holds no borrows (`where T: sendable` does).
- **report**: compile=accept-with-warning; trust lines=1
      g04_dispatched_generic_method_erases.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the 
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## g05_owned_upcast_into_stdlib_generic
> ERASURE at a standard-library generic instantiated at an interface: an `owned Account` passed where `OwningArray<IShow>.Add` takes `owned T` at T = IShow (`Shared<IShow>` is not formable: r40 and stdlib 2.16 require a proper class type, so `Shared<T>` never erases to an interface). The conversion is at the CALLER (the parameter type after substitution is the interface), so the concrete rule decides it with no generic-body requirement. EXPECT today: compiles; faults. Under 0212: error at the conversion.
- **report**: compile=accept-with-warning; trust lines=1
      g05_owned_upcast_into_stdlib_generic.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the 
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## g06_thrown_holder
> ERASURE at `throw`: an exception that holds a borrow of a scope-bound local is thrown past that local's scope and caught at a type that announces nothing. EXPECT today: compiles; one report line; checked = DisposedAccessException in the handler. Under 0212: error at the `throw` (the operand's source set is not empty and a catch site re-establishes none).
- **report**: compile=accept-with-warning; trust lines=2
      g06_thrown_holder.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must keep al
      g06_thrown_holder.bzl:23:13: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must keep al
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## g07_borrow_at_interface_is_not_erasure
> NOT an erasure: a BORROW of a holder passed at an interface type to a constructor that stores it. Under 0212 the constructed `Keeper` carries what the argument contributes — the binding `a` and the sources `a` carries — read at the caller from the argument, not from the parameter type. EXPECT today: compiles; two report lines; checked = DisposedAccessException. Under 0212: the use after `c.Dispose()` is the dominated-use error (spec 28.2), through `k`.
- **report**: compile=accept-with-warning; trust lines=2
      g07_borrow_at_interface_is_not_erasure.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source th
      g07_borrow_at_interface_is_not_erasure.bzl:23:11: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source th
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## g08_generic_box_keeps_T
> NOT an erasure: a generic box whose every owning position is declared at T. `Box<Account>` holds borrows at this instantiation (an owned field whose substituted type does), so `bx` carries `c` and no requirement arises. EXPECT today: compiles; checked = DisposedAccessException. Under 0212: admitted as a type; the use after `c.Dispose()` is the dominated-use error. The value taken out is returned `owned` from an instance member, so it carries the receiver and with it `c`. (Control for g01-g04; the prior note's p20. The bare take at a nullable field is used because r40 does not implement the replacing `move`.)
- **report**: compile=accept-with-warning; trust lines=1
      g08_generic_box_keeps_T.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must k
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## q01_two_param_store_as_scoped_method
> OPEN QUESTION (a store that relates two parameters): the helper `Feed(sink, it) => sink.Keep(it)` has no static spelling as a static method. Written as a method of the storing object with a `scoped` parameter, the bounding object is the receiver. MEASURE on r40: is a class-typed `scoped` method parameter admitted with a `using var` receiver declared after the source? (Form 5 of 28.14: "otherwise -> the receiver".)
- **report**: compile=accept; trust lines=0
  run exit=0; output: feeding|a|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: feeding|a|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: feeding|a|

## q02_two_param_store_as_extension
> OPEN QUESTION (a store that relates two parameters), the extension form: a helper declared as an extension of the storing type, with a `scoped` parameter forwarded to the type's own `scoped` member. MEASURE on r40: does 28.14 admit `scoped` on an extension method (the receiver parameter as the bounding object)?
- **report**: compile=accept; trust lines=0
  run exit=0; output: feeding|a|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: feeding|a|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: feeding|a|

## q03_static_registry
> KIND 2 in a static: a process-lived registry holds borrows of items that die earlier. EXPECT today: compiles; a report line at the collection store; checked = DisposedAccessException. Under 0212: the store adds `it` to a holder that static storage holds, and static storage admits only static-sourced borrows (28.3 Static stores): error at the store. The rewrite is q04.
- **report**: compile=accept-with-warning; trust lines=0
      q03_static_registry.bzl:11:3: warning: static readonly field `_items` has value type `Array` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§22.12 rul
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept-with-warning; trust lines=0
      q03_static_registry.bzl:11:3: warning: static readonly field `_items` has value type `Array` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§22.12 rul
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept-with-warning; trust lines=0
      q03_static_registry.bzl:11:3: warning: static readonly field `_items` has value type `Array` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§22.12 rul
  run exit=0; output: |

## q05_scoped_param_member_access
> FINDING: a member access through a class-typed `scoped` parameter inside the callee. Spec 28.14 (scoped construction, callee side) bars returning it and storing it elsewhere; it is a passive borrow and "passing it as a plain argument is an ordinary borrow argument". EXPECT (spec): compiles; prints. MEASURE r40.
- **report**: compile=REJECT; trust lines=0
      q05_scoped_param_member_access.bzl:15:36: `it` is a `scoped` borrow parameter — it may only be stored into a plain field of the bounding object `this` itself, or forwarded to a `scoped` parameter of a call on `this`; it may not 
- **checked**: compile=REJECT; trust lines=0
      q05_scoped_param_member_access.bzl:15:36: `it` is a `scoped` borrow parameter — it may only be stored into a plain field of the bounding object `this` itself, or forwarded to a `scoped` parameter of a call on `this`; it may not 
- **unchecked**: compile=REJECT; trust lines=0
      q05_scoped_param_member_access.bzl:15:36: `it` is a `scoped` borrow parameter — it may only be stored into a plain field of the bounding object `this` itself, or forwarded to a `scoped` parameter of a call on `this`; it may not 

## q06_two_param_store_extension_on_collection
> OPEN QUESTION (a store that relates two parameters): can the helper of rule case s28_11_0041, `static method Store(a: Array<Item>, it: Item)`, be written today as an extension of the standard collection with a `scoped` parameter? MEASURE on r40. RESULT: no, on two counts — an identifier in an extension head's type-argument list is a type parameter (spec 26.4), so `it` is typed at a type parameter, where `scoped` is not admitted (22.3); and r40 lets a `scoped` parameter be forwarded only to another `scoped` parameter, not to `Add`. Under 0212 the first needs `scoped` at a type parameter (the proposal's 8.7) and the second is a store into the bounding object. Today the rewrite for a standard collection is to store at the caller.
- **report**: compile=REJECT; trust lines=0
      q06_two_param_store_extension_on_collection.bzl:10:17: warning: type parameter `Item` in this extension block shadows the type `Item` that is in scope (§26.4); the extension type parameter always takes precedence
      q06_two_param_store_extension_on_collection.bzl:11:3: `scoped` is valid only on a function-typed, class-typed, or interface-typed parameter (§22.3, §28.14 v0.175/v0.180); `it` does not have any of those types
      q06_two_param_store_extension_on_collection.bzl:12:14: `it` is a `scoped` parameter — it may be forwarded only to another `scoped` parameter of a call on the same bounding object (`this`) (§28.14, v0.175)
- **checked**: compile=REJECT; trust lines=0
      q06_two_param_store_extension_on_collection.bzl:10:17: warning: type parameter `Item` in this extension block shadows the type `Item` that is in scope (§26.4); the extension type parameter always takes precedence
      q06_two_param_store_extension_on_collection.bzl:11:3: `scoped` is valid only on a function-typed, class-typed, or interface-typed parameter (§22.3, §28.14 v0.175/v0.180); `it` does not have any of those types
      q06_two_param_store_extension_on_collection.bzl:12:14: `it` is a `scoped` parameter — it may be forwarded only to another `scoped` parameter of a call on the same bounding object (`this`) (§28.14, v0.175)
- **unchecked**: compile=REJECT; trust lines=0
      q06_two_param_store_extension_on_collection.bzl:10:17: warning: type parameter `Item` in this extension block shadows the type `Item` that is in scope (§26.4); the extension type parameter always takes precedence
      q06_two_param_store_extension_on_collection.bzl:11:3: `scoped` is valid only on a function-typed, class-typed, or interface-typed parameter (§22.3, §28.14 v0.175/v0.180); `it` does not have any of those types
      q06_two_param_store_extension_on_collection.bzl:12:14: `it` is a `scoped` parameter — it may be forwarded only to another `scoped` parameter of a call on the same bounding object (`this`) (§28.14, v0.175)
