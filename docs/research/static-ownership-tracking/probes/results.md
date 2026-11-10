# Probe results — bezelc 0.0.0-a30bb11d — 2026-11-10

Modes: report = --checked-disposal --report-trust-admissions; checked = --checked-disposal; unchecked = --unchecked-disposal. 'trust lines' counts 'trust admission recorded' warnings. Exit 70 = uncaught DisposedAccessException.

## p01_record_in_class_field
> STATIC REGIME (records): a borrow-bearing record stored in a class instance field. EXPECT (spec §9.7 instance-field bullet): compile error. This is the rule candidate A would replace, for a class holder, by "the class becomes borrow-bearing".
- **report**: compile=REJECT; trust lines=0
      p01_record_in_class_field.bzl:0:0: a borrow-bearing record may not be stored in an instance field of a class — a record-carried borrow can be consumed through access paths with no per-access disposed-flag check, so the checked-m
- **checked**: compile=REJECT; trust lines=0
      p01_record_in_class_field.bzl:0:0: a borrow-bearing record may not be stored in an instance field of a class — a record-carried borrow can be consumed through access paths with no per-access disposed-flag check, so the checked-m
- **unchecked**: compile=REJECT; trust lines=0
      p01_record_in_class_field.bzl:0:0: a borrow-bearing record may not be stored in an instance field of a class — a record-carried borrow can be consumed through access paths with no per-access disposed-flag check, so the checked-m

## p02_record_outlives_source
> STATIC REGIME (records): a borrow-bearing record local declared in an outer scope takes a borrow of an inner-scope source. EXPECT (spec §9.7 first bullet + K2.3): compile error "would outlive its source". The class analogue (p17) is admitted on trust today.
- **report**: compile=REJECT; trust lines=0
      p02_record_outlives_source.bzl:16:7: borrow-bearing record assigned to `p` carries a borrow of `inner`, which is disposed first — a borrow-bearing record may not outlive the source of any borrow it contains, and `p` lives in a b
- **checked**: compile=REJECT; trust lines=0
      p02_record_outlives_source.bzl:16:7: borrow-bearing record assigned to `p` carries a borrow of `inner`, which is disposed first — a borrow-bearing record may not outlive the source of any borrow it contains, and `p` lives in a b
- **unchecked**: compile=REJECT; trust lines=0
      p02_record_outlives_source.bzl:16:7: borrow-bearing record assigned to `p` carries a borrow of `inner`, which is disposed first — a borrow-bearing record may not outlive the source of any borrow it contains, and `p` lives in a b

## p03_record_returned_param
> STATIC REGIME (records): a static method returns a borrow-bearing record carrying a plain parameter. EXPECT (spec §9.7 return bullet): compile error at the return. The class analogue — a factory returning `new Account(c)` — is admitted on trust today (k3d).
- **report**: compile=REJECT; trust lines=0
      p03_record_returned_param.bzl:12:47: returned borrow-bearing record carries a borrow of parameter `i` — a borrow-bearing record may not outlive the source of any borrow it carries, and a plain parameter of the returning method h
- **checked**: compile=REJECT; trust lines=0
      p03_record_returned_param.bzl:12:47: returned borrow-bearing record carries a borrow of parameter `i` — a borrow-bearing record may not outlive the source of any borrow it carries, and a plain parameter of the returning method h
- **unchecked**: compile=REJECT; trust lines=0
      p03_record_returned_param.bzl:12:47: returned borrow-bearing record carries a borrow of parameter `i` — a borrow-bearing record may not outlive the source of any borrow it carries, and a plain parameter of the returning method h

## p04_record_source_moved
> STATIC REGIME (records): the source of a live borrow-bearing record local is transferred (a silent kill). EXPECT (spec K5.1 through K5.5's holders): compile error at the move. The class analogue is k3b: admitted today, faults.
- **report**: compile=REJECT; trust lines=0
      p04_record_source_moved.bzl:16:15: cannot consume `i` here: the borrow held by `p` is still live — its later use would read freed memory (consume-site freeze, §28.3/§28.7/§30.14 v0.180)
- **checked**: compile=REJECT; trust lines=0
      p04_record_source_moved.bzl:16:15: cannot consume `i` here: the borrow held by `p` is still live — its later use would read freed memory (consume-site freeze, §28.3/§28.7/§30.14 v0.180)
- **unchecked**: compile=REJECT; trust lines=0
      p04_record_source_moved.bzl:16:15: cannot consume `i` here: the borrow held by `p` is still live — its later use would read freed memory (consume-site freeze, §28.3/§28.7/§30.14 v0.180)

## p05_record_source_disposed_dominated
> STATIC REGIME (records): the source of a borrow-bearing record local is explicitly disposed, then the record is used (the kill dominates the use). EXPECT (spec §28.2, proposal 0205's holder rule): compile error at the use. The class analogue is k3a: admitted today, faults on every run.
- **report**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=139; output: 

## p06_record_erased_at_interface
> STATIC REGIME (records): a borrow-bearing record reaches an `Object`-typed parameter (a borrow-erasing position). EXPECT (spec §9.7 borrow-erasing bullet): compile error at the conversion. Candidate A needs the class analogue only at OWNING positions (an owned upcast), see p14.
- **report**: compile=REJECT; trust lines=0
      p06_record_erased_at_interface.bzl:21:12: the expression has type `Pair`, a borrow-bearing type, and may not be converted to `Object`: a value that holds borrows may not reach a borrow-erasing position — a position declared `Obj
- **checked**: compile=REJECT; trust lines=0
      p06_record_erased_at_interface.bzl:21:12: the expression has type `Pair`, a borrow-bearing type, and may not be converted to `Object`: a value that holds borrows may not reach a borrow-erasing position — a position declared `Obj
- **unchecked**: compile=REJECT; trust lines=0
      p06_record_erased_at_interface.bzl:21:12: the expression has type `Pair`, a borrow-bearing type, and may not be converted to `Object`: a value that holds borrows may not reach a borrow-erasing position — a position declared `Obj

## p07_alias_dispose_dominated
> BASELINE (0205): a local alias used after a dominating explicit disposal of its source. EXPECT (spec §28.2, v0.355): compile error at the use. Control for p05 (the same rule through a record holder).
- **report**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=139; output: 

## p10_scoped_plain_var_holder
> REACH OF scoped: the holder is a plain `var`, not a `using var`. EXPECT (spec §28.14: a bounding object that is no `using var` local admits nothing): compile error at the scoped argument. Candidate A tracks any holder binding, so this shape is admitted and bounded.
- **report**: compile=REJECT; trust lines=0
      p10_scoped_plain_var_holder.bzl:20:33: a capture-bearing closure is not admitted here: the call's bounding object does not resolve to a `using var`/`await using var` local or to `this` (§28.14 — a plain expression, a borrow-fie
- **checked**: compile=REJECT; trust lines=0
      p10_scoped_plain_var_holder.bzl:20:33: a capture-bearing closure is not admitted here: the call's bounding object does not resolve to a `using var`/`await using var` local or to `this` (§28.14 — a plain expression, a borrow-fie
- **unchecked**: compile=REJECT; trust lines=0
      p10_scoped_plain_var_holder.bzl:20:33: a capture-bearing closure is not admitted here: the call's bounding object does not resolve to a `using var`/`await using var` local or to `this` (§28.14 — a plain expression, a borrow-fie

## p11_scoped_factory_form2
> REACH OF scoped: a static factory forwards its scoped parameter into the constructor and returns the holder owned; the caller binds it to a `using var` (bounding form 2). EXPECT (spec §28.14): compiles; no report line; static bound.
- **report**: compile=accept; trust lines=0
  run exit=0; output: ann|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: ann|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: ann|

## p12_scoped_holder_in_owned_field
> REACH OF scoped: a two-level holder. Branch's constructor takes a scoped Customer and builds an Account over it, held in an owned field of the Branch. EXPECT (spec §28.14: a `scoped` borrow is stored only in a direct plain field of the bounding object or forwarded to a `scoped` parameter with the same bounding object; "storage reached through `owned` fields of the bounding object is excluded"): compile error. So the nested holder has no static spelling today; its trust form is p13. Candidate A admits it: Branch owns a borrow-bearing Account, so Branch is borrow-bearing and carries `c`.
- **report**: compile=REJECT; trust lines=0
      p12_scoped_holder_in_owned_field.bzl:20:33: `c` is a `scoped` borrow parameter — it may only be stored into a plain field of the bounding object `this` itself, or forwarded to a `scoped` parameter of a call on `this`; it may not
- **checked**: compile=REJECT; trust lines=0
      p12_scoped_holder_in_owned_field.bzl:20:33: `c` is a `scoped` borrow parameter — it may only be stored into a plain field of the bounding object `this` itself, or forwarded to a `scoped` parameter of a call on `this`; it may not
- **unchecked**: compile=REJECT; trust lines=0
      p12_scoped_holder_in_owned_field.bzl:20:33: `c` is a `scoped` borrow parameter — it may only be stored into a plain field of the bounding object `this` itself, or forwarded to a `scoped` parameter of a call on `this`; it may not

## p13_trust_holder_in_owned_field
> TRUST TODAY: the two-level holder with plain parameters; the source is disposed while the outer holder lives. A per-parameter "retains" bit sees Account's constructor, but Branch's constructor stores no plain parameter of its own into a field: it stores an OWNED Account. EXPECT (spec): compiles; one report line (Account's store); DisposedAccessException in checked mode. Candidate A: Branch owns a borrow-bearing Account, so Branch is borrow-bearing and `b` carries `c`; the dominated use is a compile error.
- **report**: compile=accept-with-warning; trust lines=1
      p13_trust_holder_in_owned_field.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the calle
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p14_trust_holder_owned_upcast
> TRUST TODAY, the interface crux: a holder is returned OWNED at an interface type, so its static type announces no borrow. EXPECT (spec): compiles; one report line; DisposedAccessException in checked mode. Candidate A: the owned conversion of a borrow-bearing instance to a type that is not borrow-bearing is the borrow-erasing error (or the interface must be declared borrow-bearing).
- **report**: compile=accept-with-warning; trust lines=1
      p14_trust_holder_owned_upcast.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller 
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p15_trust_holder_reachable_from_static
> TRUST TODAY: a holder of a local's borrow is moved into an object a static references (the static outlives every local). EXPECT (spec K4.3: "a borrow held inside an object a static references is that object's field-held clause"): compiles; DisposedAccessException in checked mode at the later read. Candidate A: `Vault` owns a borrow-bearing Account, so it is borrow-bearing; the store adds a non-static source to a value held by static storage: compile error at the call.
- **report**: compile=accept-with-warning; trust lines=1
      p15_trust_holder_reachable_from_static.bzl:25:3: warning: static readonly field `Vault` has class type `Vault` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchroniz
      p15_trust_holder_reachable_from_static.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source th
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept-with-warning; trust lines=0
      p15_trust_holder_reachable_from_static.bzl:25:3: warning: static readonly field `Vault` has class type `Vault` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchroniz
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept-with-warning; trust lines=0
      p15_trust_holder_reachable_from_static.bzl:25:3: warning: static readonly field `Vault` has class type `Vault` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchroniz
  run exit=0; output: |

## p16_holder_not_sendable
> STATIC TODAY: a class with a plain class-typed field is not sendable (spec §30.14 rule 3), so a holder cannot be moved to another domain. EXPECT: compile error naming the property. Candidate A changes nothing here; "borrow-bearing" and "not transfer-closed" coincide on this rule.
- **report**: compile=REJECT; trust lines=1
      p16_holder_not_sendable.bzl:25:39: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sea
      p16_holder_not_sendable.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must k
- **checked**: compile=REJECT; trust lines=0
      p16_holder_not_sendable.bzl:25:39: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sea
- **unchecked**: compile=REJECT; trust lines=0
      p16_holder_not_sendable.bzl:25:39: Domain.Spawn argument is not sendable: argument types to a spawned domain must satisfy §30.14 sendable-type rules (not borrow-bearing, not function-typed, class types must be transfer-closed/sea

## p17_trust_method_store_inner_source
> TRUST TODAY, kind 2 in miniature: a method (not a constructor) re-points the holder's borrow at an inner-scope source; the holder is read after that scope ends. EXPECT (spec): compiles; one report line; DisposedAccessException in checked mode. Candidate A: the store adds `inner` to the holder's source set and is checked as an assignment to the holder's binding: "would outlive its source" (p02's rule).
- **report**: compile=accept-with-warning; trust lines=2
      p17_trust_method_store_inner_source.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the c
      p17_trust_method_store_inner_source.bzl:15:56: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the c
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p18_conditional_dispose_local_borrow
> CANDIDATE D: an explicit Dispose() on one path while a local borrow is live, then a use through the borrow. EXPECT (spec K4.2/K5.3: the announced kill is unfrozen; the use is not dominated, so 0205 does not reach it): compiles; DisposedAccessException in checked mode when the branch runs. A stricter freeze would reject the Dispose().
- **report**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p19_conditional_dispose_under_holder
> CANDIDATES A/B/D: the same conditional kill, with the borrow held by a class holder. EXPECT (spec): compiles; DisposedAccessException in checked mode when the branch runs. Under A or B the holder is a traced borrow, and the conditional announced kill stays the flag's (K4.2) unless the stricter freeze (D) is also taken.
- **report**: compile=accept-with-warning; trust lines=1
      p19_conditional_dispose_under_holder.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the 
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p20_trust_generic_box_of_holder
> TRUST TODAY, generics: a generic owning box instantiated at a holder class. EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: Box<Account> is borrow-bearing at this instantiation (an owned field whose substituted type is borrow-bearing), so `bx` carries `c`.
- **report**: compile=accept-with-warning; trust lines=1
      p20_trust_generic_box_of_holder.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the calle
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p21_trust_owning_array_of_holders_returned
> TRUST TODAY: holders collected in an owning collection and returned; the source is a local of the caller that is transferred away while the collection lives (a silent kill no freeze sees). EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: OwningArray<Account> is borrow-bearing at this instantiation and the returned value carries the call's plain argument `c`: the move is frozen (K5.1).
- **report**: compile=accept-with-warning; trust lines=1
      p21_trust_owning_array_of_holders_returned.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose sourc
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p22_ctor_extra_argument
> PRECISION OF A (constructor arguments): the holder's constructor takes two class-typed arguments and keeps only the first; the second is disposed while the holder lives. EXPECT (spec): compiles; runs clean (no stale read exists). Under A with "a construction carries every plain reference argument" the use after `o.Dispose()` is rejected (an over-rejection); under A with the stored parameter marked (`scoped`) it is accepted.
- **report**: compile=accept-with-warning; trust lines=2
      p22_ctor_extra_argument.bzl:21:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must k
      p22_ctor_extra_argument.bzl:22:14: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must k
  run exit=0; output: ann|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: ann|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: ann|

## p23_backpointer_tree_in_static
> KIND 1 under A: an owner whose owned child points back at it, held in a static. The only borrow inside the tree is of the tree itself. EXPECT (spec): compiles; one report line; prints Root. Under A the Owner class is borrow-bearing by type, but a value constructed from no borrowed argument has an EMPTY source set, so the static store is admitted (the class analogue of §9.7's null-source form).
- **report**: compile=accept-with-warning; trust lines=1
      p23_backpointer_tree_in_static.bzl:21:3: warning: static readonly field `Root` has class type `Owner` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§
      p23_backpointer_tree_in_static.bzl:16:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller
  run exit=0; output: Root|
- **checked**: compile=accept-with-warning; trust lines=0
      p23_backpointer_tree_in_static.bzl:21:3: warning: static readonly field `Root` has class type `Owner` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§
  run exit=0; output: Root|
- **unchecked**: compile=accept-with-warning; trust lines=0
      p23_backpointer_tree_in_static.bzl:21:3: warning: static readonly field `Root` has class type `Owner` which is reachable from every domain — a warning: confine its use to one domain or ensure its own internal synchronization (§
  run exit=0; output: Root|

## p24_trust_shared_holder
> TRUST TODAY: a holder moved into a Shared<T> handle; the source is disposed while the handle lives. EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: Shared<Account> is borrow-bearing at this instantiation; the handle and every Clone() carry `c`.
- **report**: compile=accept-with-warning; trust lines=1
      p24_trust_shared_holder.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the caller must k
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p25_closure_field_store_rejected
> STATIC REGIME (function values): a capture-bearing closure stored in a field through a plain parameter. EXPECT (spec §28.14 call-bounded default): compile error in the callee — a plain function-typed parameter is never stored. This is the posture candidate A1 gives a plain CLASS-typed parameter.
- **report**: compile=REJECT; trust lines=0
      p25_closure_field_store_rejected.bzl:0:0: parameter `f` is call-bounded; declare it `scoped` to store it (§28.14, v0.177) — a plain function-typed parameter may be invoked, bound to locals, and passed to another plain function-
- **checked**: compile=REJECT; trust lines=0
      p25_closure_field_store_rejected.bzl:0:0: parameter `f` is call-bounded; declare it `scoped` to store it (§28.14, v0.177) — a plain function-typed parameter may be invoked, bound to locals, and passed to another plain function-
- **unchecked**: compile=REJECT; trust lines=0
      p25_closure_field_store_rejected.bzl:0:0: parameter `f` is call-bounded; declare it `scoped` to store it (§28.14, v0.177) — a plain function-typed parameter may be invoked, bound to locals, and passed to another plain function-

## p26_iterator_frame_traced
> STATIC REGIME (frames): an iterator frame held in a local carries its plain argument; the argument is moved away while the frame is live. EXPECT (spec K6.4 + K5.5): compile error at the move. A frame is the existing precedent for "a class-typed value that carries a source set" (candidate A's holder).
- **report**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p27_holder_moved_then_source_killed
> TRUST TODAY, the moved holder: the holder is moved into another binding (and could be passed on), then the source is transferred away. A caller-side rule keyed on "the holder is a `using var`" (0132's v1 scope, §28.14's bounding forms) does not follow the move. EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: the source set travels with the value; the transfer of `c` is frozen while `b` is live (K5.1).
- **report**: compile=accept-with-warning; trust lines=1
      p27_holder_moved_then_source_killed.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose source the c
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **checked**: compile=accept; trust lines=0
  run exit=70; output: Unhandled exception: System.DisposedAccessException|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: |

## p28_registry_weak_today
> KIND 2 under A: a long-lived registry declared BEFORE the items it observes cannot hold tracked borrows of them (p17's rule). The safe spelling that exists is Shared<T> + Weak<T>. EXPECT (spec §28.13): compiles; no report line; prints "gone" then "a"; never faults in either build mode.
- **report**: compile=accept; trust lines=0
  run exit=0; output: gone|a|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: gone|a|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: gone|a|

## p29_registry_scoped_reordered
> KIND 2 -> KIND 3 by declaration order: the registry is declared AFTER the items, so it dies first. Spelled with today's `scoped` parameter on a method (bounding form 5: the receiver, a `using var`). EXPECT (spec §28.14): compiles; no report line. Under A the same program needs no marker at the call beyond what A's surface asks, and the same order is what the scope comparison checks.
- **report**: compile=accept; trust lines=0
  run exit=0; output: a|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: a|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: a|

## p30_registry_scoped_wrong_order
> KIND 2 with today's `scoped`: the holder is declared BEFORE the source. EXPECT (spec §28.14 declared-before test): compile error at the scoped argument. This is the rejection candidate A gives every tracked holder; the program must reorder, own the item, or use Weak<T>.
- **report**: compile=REJECT; trust lines=0
      p30_registry_scoped_wrong_order.bzl:21:21: captured source `a` is not declared before the bounding `using var` `w` — every captured source must be declared before the bounding object so it provably outlives every invocation of t
- **checked**: compile=REJECT; trust lines=0
      p30_registry_scoped_wrong_order.bzl:21:21: captured source `a` is not declared before the bounding `using var` `w` — every captured source must be declared before the bounding object so it provably outlives every invocation of t
- **unchecked**: compile=REJECT; trust lines=0
      p30_registry_scoped_wrong_order.bzl:21:21: captured source `a` is not declared before the bounding `using var` `w` — every captured source must be declared before the bounding object so it provably outlives every invocation of t

## p31_rewrite_temporary_source_bound
> REWRITE of the temporary-source shape (corpus case s22_6_2312), form 1: the array literals are bound to locals declared before the holders, so each source outlives its holder. EXPECT: compiles; prints 15. Under A this is admitted with no marker (the constructor mints; `a` is declared before `r1`).
- **report**: compile=accept; trust lines=0
  run exit=0; output: 15|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: 15|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: 15|

## p32_rewrite_temporary_source_owned
> REWRITE of the temporary-source shape (corpus case s22_6_2312), form 2: the holder OWNS its items (an owned field and an owned constructor parameter), so the literal is transferred, not borrowed. EXPECT: compiles; prints 15; no report line. Under A the class holds no borrow.
- **report**: compile=accept; trust lines=0
  run exit=0; output: 15|
- **checked**: compile=accept; trust lines=0
  run exit=0; output: 15|
- **unchecked**: compile=accept; trust lines=0
  run exit=0; output: 15|
