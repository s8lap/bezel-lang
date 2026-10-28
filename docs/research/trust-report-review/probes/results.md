# Probe results — bezelc 0.0.0-f6863e62 — 2026-10-28

Legend: verdict = accept | accept-with-warning | REJECT; 'trust lines' = count of 'trust admission recorded' warnings under the flag; the default-build column is the same program with no flag.

## a01_param_into_field
> Family A, §28.3 field-store clause 3 (s28_3_0090 shape): plain ctor parameter into a plain field. EXPECT: report.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      a01_param_into_field.bzl:10:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter who
- **noflag**: compile=accept; trust lines=0

## a02_scoped_twin
> §28.14 scoped construction twin. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## a03_fieldpath_into_field
> Family A: a FIELD-PATH-sourced borrow (`owner.Child`, a property read through a parameter) stored into a plain field.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      a03_fieldpath_into_field.bzl:18:14: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter
- **noflag**: compile=accept; trust lines=0

## a04_receiver_fieldpath_into_field
> Family A: a receiver-rooted field path (`this._child`) stored into another plain field of this. EXPECT: report (receiver-sourced).
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## a05_property_store
> Family A: §28.3 "Property stores of a borrow-carrying value": `obj.P := param` is judged as the field store and admitted under the residual. EXPECT: report.
- **flag**: compile=accept-with-warning; trust lines=2
  diagnostics (first line of each, cut):
      a05_property_store.bzl:10:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose
      a05_property_store.bzl:19:19: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose
- **noflag**: compile=accept; trust lines=0

## a06_primary_ctor_capture
> Family A: a primary-constructor parameter read from a body captures it into a compiler-generated field (§22.10) — a field-held borrow. EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## a07_funcvalue_invocation
> Family A: `_v := make()` with `make: () -> Item` — admitted under clause 3's residual (spec §28.3 clause 2 text names it). EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## a08_local_rejected_control
> Control: a scope-bound local stored into a field is REJECTED (no report, an error). EXPECT: error.
- **flag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
- **noflag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):

## a09_value_param_control
> Control: a value-typed parameter stored into a field carries no borrow. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## a10_interface_param
> Family A: an interface-typed plain parameter stored into an interface-typed plain field. EXPECT: report.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      a10_interface_param.bzl:9:14: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter whose
- **noflag**: compile=accept; trust lines=0

## a11_owned_field_as_plain_arg
> §28.6 restatement: an owned field passed as a plain argument that the callee stores. The admission is in Account's constructor (reported there, a01); the caller side has no site. EXPECT: one report, in Account.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      a11_owned_field_as_plain_arg.bzl:10:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) param
- **noflag**: compile=accept; trust lines=0

## a12_field_init_from_param
> Family A: field INITIALIZER reading a primary-constructor parameter (`private _o: Customer := owner;`). EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## a13_this_field_into_field
> Spelling twin of a04: `_alias := this._child;` (member-access spelling of the same receiver-rooted field path). Compare with a04's bare `_child`.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      a13_this_field_into_field.bzl:12:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) paramete
- **noflag**: compile=accept; trust lines=0

## b01_param_add_field_array
> Family B (s28_3_0093): parameter borrow Added to a field-held borrowing Array. EXPECT: report.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      b01_param_add_field_array.bzl:13:16: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) p
- **noflag**: compile=accept; trust lines=0

## b02_this_and_field_add
> Family B: receiver-sourced (`this`) and field-sourced (`_owner`) elements stored into a field-held borrowing collection. EXPECT: two reports.
- **flag**: compile=accept-with-warning; trust lines=2
  diagnostics (first line of each, cut):
      b02_this_and_field_add.bzl:7:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter wh
      b02_this_and_field_add.bzl:11:16: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) para
- **noflag**: compile=accept; trust lines=0

## b03_unclassified_receiver
> Family B: a store whose receiver the structural check leaves unclassified (the result of a call). EXPECT: report ("receiver unclassified").
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      b03_unclassified_receiver.bzl:14:17: warning: trust admission recorded (§28.11 borrowing-collection store (receiver unclassified)): the compiler is admitting a borrow of the receiver or a plain (non-
- **noflag**: compile=accept; trust lines=0

## b04_owningdictionary_key
> Family B: the KEY slot of an OwningDictionary<K, V> is a borrowing position (§28.11 "the same check reaches the key slot"). EXPECT: report at the key.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## b05_indexer_set
> Family B: the indexer-set store of a parameter borrow into a field-held borrowing Array. EXPECT: report.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      b05_indexer_set.bzl:11:16: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter w
- **noflag**: compile=accept; trust lines=0

## b06_other_stores
> Family B: Insert / Push / Enqueue / AddFirst / Dictionary.Add value slot, each with a parameter borrow into a field-held borrowing collection. EXPECT: five reports.
- **flag**: compile=accept-with-warning; trust lines=5
  diagnostics (first line of each, cut):
      b06_other_stores.bzl:24:5: warning: the result of this expression statement is unused — a non-`()` result is silently discarded, which may indicate a bug (a return value the programmer forgot to use
      b06_other_stores.bzl:21:18: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter 
      b06_other_stores.bzl:22:13: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter 
      b06_other_stores.bzl:23:16: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter 
      b06_other_stores.bzl:24:17: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter 
      b06_other_stores.bzl:25:15: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) parameter 
- **noflag**: compile=accept-with-warning; trust lines=0
  diagnostics (first line of each, cut):
      b06_other_stores.bzl:24:5: warning: the result of this expression statement is unused — a non-`()` result is silently discarded, which may indicate a bug (a return value the programmer forgot to use

## b07_local_collection_quiet
> Control (0131 §3 row): a purely local collection the parameter outlives. EXPECT: quiet.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      b07_local_collection_quiet.bzl:11:13: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) 
- **noflag**: compile=accept; trust lines=0

## b08_transfer_entry_0157
> Proposal 0157 (v0.333) transfer entry (s28_3_0162): a local held by a local collection, then transferred. EXPECT: report at `Take(move item)`.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## b09_transfer_entry_dispose_quiet
> s28_3_0166: disposing a collection-held local is a flag-backstopped kill. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## b10_fresh_store
> Currency: 0131 §3 row says "§15.4's fresh-store WARNING — in both builds". Since v0.346 (proposal 0207) it is an ERROR. EXPECT (spec v0.349): error. r35: measure.
- **flag**: compile=accept-with-warning; trust lines=0
  diagnostics (first line of each, cut):
      b10_fresh_store.bzl:11:16: warning: this `.Add(...)` store on `_conns` stores a fresh unbound owned temporary into a borrowing-family collection: the argument dies at its K8 full-expression boundary (
- **noflag**: compile=accept-with-warning; trust lines=0
  diagnostics (first line of each, cut):
      b10_fresh_store.bzl:11:16: warning: this `.Add(...)` store on `_conns` stores a fresh unbound owned temporary into a borrowing-family collection: the argument dies at its K8 full-expression boundary (

## b11_comparer_held_param
> Comparer-held clause (v0.327, proposal 0138 §7, #5604): a comparer passed to a hash collection's constructor is retained as a borrow; the constructor call is the report site. EXPECT: report at `new Dictionary<string, int>(cmp)`.
- **flag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
- **noflag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):

## b12_comparer_held_local
> Comparer-held clause with a scope-bound LOCAL comparer and a field-held dictionary: the retained borrow dangles after the constructor. Spec: the retention is read off the signature ("take a plain parameter and retain it") and is recorded trust. EXPECT: report (or a rejection if the checker sees the store); measure.
- **flag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
- **noflag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):

## b13_param_collection_receiver
> Family B: a store into a collection held by a PLAIN PARAMETER (caller storage) of a parameter-sourced borrow: admitted under the residual. EXPECT: report.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      b13_param_collection_receiver.bzl:9:14: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`
- **noflag**: compile=accept; trust lines=0

## b14_owned_param_collection_local_elem
> §28.11 clause 3: a collection an `owned` parameter holds is the body's own; a store of a top-level local's borrow into it is "admitted under the residual". The element is body-sourced, so the 0157 site rule applies: report only at a transfer of `item`; here there is none. EXPECT: quiet (both v0.349 readings agree: no transfer).
- **flag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
- **noflag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):

## b15_addrange_param
> Family B: AddRange of a parameter-held collection into a field-held borrowing collection — every element is parameter-sourced (value-copy member (ii)). EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## b16_this_field_add_spelling
> Spelling twin of b02's missed line: `_peers.Add(this._owner)` vs the bare `_peers.Add(_owner)`. EXPECT: both report (field-path-sourced element).
- **flag**: compile=accept-with-warning; trust lines=2
  diagnostics (first line of each, cut):
      b16_this_field_add_spelling.bzl:7:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`) paramet
      b16_this_field_add_spelling.bzl:12:16: warning: trust admission recorded (§28.11 borrowing-collection store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`scoped`)
- **noflag**: compile=accept; trust lines=0

## c01_fireandforget_receiver
> Family C (s28_3_0095): Task.FireAndForget of a task minted over a plain-parameter RECEIVER. EXPECT: report.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      c01_fireandforget_receiver.bzl:13:24: warning: trust admission recorded (§30.1 pending-frame settlement obligation): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`s
- **noflag**: compile=accept; trust lines=0

## c02_awaited_twin
> Family C control (s28_3_0096): awaited in the sources' scope. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c03_fireandforget_static_arg
> Family C: the SPEC'S OWN §30.1 example — a static async method whose plain ARGUMENT `c` is held in the frame, abandoned by Task.FireAndForget. EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c04_fireandforget_instance_arg
> Family C: an instance async method on a field-sourced receiver with a plain-parameter ARGUMENT, abandoned. EXPECT: report (the argument `c` is a held source even though the receiver is `this`-rooted).
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      c04_fireandforget_instance_arg.bzl:13:24: warning: trust admission recorded (§30.1 pending-frame settlement obligation): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, no
- **noflag**: compile=accept; trust lines=0

## c05_task_stored_in_field
> Family C: a minted task STORED into an owned field — the frame outlives the call with a borrowed receiver (§28.3 report text: "a task ... stored"). EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c06_bound_not_awaited_every_path
> Family C: a task bound and NOT awaited on every path before a held source's scope exits (0131 Q3: spec-ahead). EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c07_bound_awaited_quiet
> Family C control (0131 §3 row): `using var t := c.PingAsync(); await t;` awaited in scope. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c08_patternA_plain_operand
> Family C: Pattern A await of a plain BORROWED task operand (§30.5's true-task-outlives-the-use residual). EXPECT: report at the await.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c09_return_await_owned_field
> Family C: `return await _task;` over an owned field of `this` read without `move` (§30.5 kind 3, non-static source). EXPECT: report.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c10_task_added_to_owning_collection
> Family C: a minted task over a borrowed receiver stored into an owning collection field. EXPECT: report (the frame is held past the call).
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c11_mint_announcing_return_quiet
> 0131 §1.2 exclusion: a frame returned under a mint-announcing type is type-announced. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c12_taskscope_spawn_quiet
> Family C remedy: TaskScope.Spawn with a `scoped` body — the scope's disposal quiesces its children; a compiler-known bounding type. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## c13_fireandforget_lambda_capture
> Family C: an async lambda capturing a plain parameter, invoked and abandoned. EXPECT: report (the capture-set contribution rides the frame).
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## d01_form3_registration_residual
> Family D: form-3 bounding (an `owned` return assigned to an owned field of `this`) — the registration-lifetime residual. EXPECT: report at the Subscribe call.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## d02_scoped_field_quiescence
> Family D: a user `scoped` field — the quiescence obligation. EXPECT: report at the declaration.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## d03_form2_using_var_quiet
> Family D control: form 2 (a `using var` local bounds the subscription) — fully static, no residual. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## d04_scoped_construction_quiet
> s28_14_0032: scoped construction over a field's current value. EXPECT: quiet.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## e01_k42_dispose_while_borrowed_quiet
> K4.2 site-announced kill (conditional, so not 0205's error). EXPECT: quiet under the mode.
- **flag**: compile=accept; trust lines=0
- **noflag**: compile=accept; trust lines=0

## e02_slot_kill_by_move_transfer_7434
> #7434 / bzcore F1: a local borrow of a slot's value, the replacing move (nullable-field bare form, §28.8.1 detach), a domain transfer, then a use. K4.3's slot-kill-by-move clause (§28.3 Scope clause 1) is "a recorded obligation" with no holder store to report at. EXPECT (spec): accepted; no report site today. Measure r35.
- **flag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
- **noflag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):

## e03_slot_kill_by_move_local_use
> Variant of e02 inside ONE body (the shape 0206's window analysis sees): borrow of the slot's value, the replacing move of the same field, transfer of the taken value, use of the stale local borrow. Measure r35 (verdict, report, run).
- **flag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
- **noflag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):

## e04_static_field_warning
> §22.12 rule 4 statics obligation: its own declaration-site warning in both builds; not a trust line. EXPECT: the §22.12 warning only.
- **flag**: compile=accept-with-warning; trust lines=0
  diagnostics (first line of each, cut):
      e04_static_field_warning.bzl:8:3: warning: static readonly field `_shared` has class type `Widget` which is reachable from every domain — a warning: confine its use to one domain or ensure its own i
- **noflag**: compile=accept-with-warning; trust lines=0
  diagnostics (first line of each, cut):
      e04_static_field_warning.bzl:8:3: warning: static readonly field `_shared` has class type `Widget` which is reachable from every domain — a warning: confine its use to one domain or ensure its own i

## e05_field_held_then_caller_disposes_run
> Run probe for the #4602 free-at-Dispose amendment: a field-held trust admission; the caller disposes the source early (K4.2) while the holder lives; a later read through the holder must be the K4.1 deterministic fault, not a freed read. EXPECT (spec): DisposedAccessException in the default build.
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      e05_field_held_then_caller_disposes_run.bzl:13:15: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`sco
  run exit=70; output:
      Unhandled exception: System.DisposedAccessException
- **noflag**: compile=accept; trust lines=0
  run exit=70; output:
      Unhandled exception: System.DisposedAccessException

## e06_field_held_certain_dangle_temporary
> Candidate: `holder.Hold(new Item())` — an unbound temporary passed to a plain parameter the callee stores (§28.3 clause 3: "admitted at the call ... a temporary meets the obligation only when the object dies within the same full expression"). Here `holder` outlives the statement and is later used: a certain dangle in the CALLER. Measure r35 (verdict, report, run).
- **flag**: compile=accept-with-warning; trust lines=1
  diagnostics (first line of each, cut):
      e06_field_held_certain_dangle_temporary.bzl:14:14: warning: trust admission recorded (§28.3 borrowed-field store): the compiler is admitting a borrow of the receiver or a plain (non-`owned`, non-`sco
  run exit=3; output:
- **noflag**: compile=accept; trust lines=0
  run exit=3; output:

## e09_replacing_move_with_parse
> The replacing move `move _f with new Q()` at a non-nullable field, as the spec writes it (§28.8.1). Measure r35's parse (the #5923 note).
- **flag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
- **noflag**: compile=REJECT; trust lines=0
  diagnostics (first line of each, cut):
