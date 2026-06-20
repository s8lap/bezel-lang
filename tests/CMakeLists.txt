# Test executable. Phase tests are added to BEZEL_TEST_SOURCES as each milestone
# lands its spec-derived tests; the per-phase directories alongside this file
# are placeholders for them.
set(BEZEL_TEST_SOURCES
    test_main.cpp
    sanity_test.cpp
    lexer/identifiers_test.cpp
    lexer/keywords_test.cpp
    lexer/comments_test.cpp
    lexer/whitespace_test.cpp
    lexer/punctuators_test.cpp
    lexer/numeric_literals_test.cpp
    lexer/char_literals_test.cpp
    lexer/string_literals_test.cpp
    lexer/verbatim_strings_test.cpp
    lexer/locations_test.cpp
    parser/module_decl_test.cpp
    parser/imports_test.cpp
    parser/empty_type_decls_test.cpp
    parser/named_types_test.cpp
    parser/generics_and_inheritance_test.cpp
    parser/field_decls_test.cpp
    parser/expressions_primary_test.cpp
    parser/expressions_binary_test.cpp
    parser/expressions_unary_test.cpp
    parser/method_decls_test.cpp
    parser/statements_basic_test.cpp
    parser/statements_control_flow_test.cpp
    parser/expressions_new_and_postfix_test.cpp
    parser/expressions_more_binary_test.cpp
    parser/expressions_typetest_and_in_test.cpp
    parser/conditional_and_throw_test.cpp
    parser/for_stmt_test.cpp
    parser/ctor_dtor_test.cpp
    parser/enum_constants_test.cpp
    parser/record_positional_test.cpp
    parser/try_catch_test.cpp
    parser/inc_discard_yield_test.cpp
    parser/param_mods_and_constraints_test.cpp
    parser/extensions_and_externs_test.cpp
    parser/property_and_const_test.cpp
    parser/alias_generics_and_operators_test.cpp
    parser/empty_param_lambdas_test.cpp
    parser/typeof_nameof_test.cpp
    parser/await_and_move_test.cpp
    parser/class_primary_ctor_test.cpp
    parser/indexer_test.cpp
    parser/event_decl_test.cpp
    parser/interface_members_test.cpp
    parser/await_using_and_for_test.cpp
    parser/parameterised_lambdas_test.cpp
    parser/property_accessor_blocks_test.cpp
    parser/property_setter_expr_body_676_test.cpp
    parser/iface_indexer_owned_setter_681_test.cpp
    parser/case_and_patterns_test.cpp
    parser/case_stmt_test.cpp
    parser/case_scrutineeless_test.cpp
    parser/nested_types_test.cpp
    parser/serialized_clause_test.cpp
    parser/serialized_class_clause_test.cpp
    parser/catch_patterns_test.cpp
    parser/constant_alternatives_test.cpp
    parser/collection_literals_test.cpp
    parser/type_extensions_test.cpp
    parser/enum_members_test.cpp
    parser/auto_accessor_vis_test.cpp
    parser/arg_modifiers_test.cpp
    parser/with_and_unit_value_test.cpp
    parser/interpolated_strings_test.cpp
    parser/range_extensions_test.cpp
    parser/local_const_test.cpp
    parser/modifier_constraint_test.cpp
    parser/index_arglist_test.cpp
    parser/e2e_kitchen_sink_test.cpp
    parser/node_spans_test.cpp
    parser/bug_468_ne_operator_test.cpp
    parser/nullable_function_type_584_test.cpp
    parser/lambda_conditional_body_589_test.cpp
    parser/catch_arm_expr_body_588_test.cpp
    sema/symbol_collection_test.cpp
    sema/static_init_cycles_797_test.cpp
    sema/groupby_store_gate_755_test.cpp
    sema/type_resolution_test.cpp
    sema/expr_literal_types_test.cpp
    # --- fix #743: §21.9 owned-value discard-as-statement is an error ---
    sema/owned_discard_stmt_743_test.cpp
    # --- end #743 ---
    # --- fix #746: R41 missing diagnostics (slice §15.6, try §31.2, interp §18.5) ---
    sema/r41_missing_diagnostics_746_test.cpp
    # --- fix #746: generic-method class/value/notnull constraint at call site (§14.2) ---
    sema/generic_method_constraint_callsite_746_test.cpp
    # --- fix #746 (R41 C): operator-overload group completeness (§25.2) ---
    sema/operator_groups_746_test.cpp
    sema/string_concat_746_test.cpp
    # --- end #746 ---
    # --- fix #747 (R42): `yield` in a lambda rejected (§29.9) ---
    sema/yield_in_lambda_747_test.cpp
    # --- fix #747 (R42 FALSE_ACCEPT): assignment to a const member rejected (§10) ---
    sema/const_assignment_747_test.cpp
    # --- fix #747 (R42 FALSE_ACCEPT): duplicate top-level type names rejected (§11.3) ---
    sema/duplicate_top_level_type_747_test.cpp
    # --- fix #747 (R42): static indexers rejected (§23.5) ---
    sema/static_indexer_747_test.cpp
    # --- fix #747 (R42): assignment to a setterless property rejected (§23.3) ---
    sema/getter_only_property_747_test.cpp
    # --- fix #747 (R42): readonly field write via non-this receiver rejected (§22.8) ---
    sema/readonly_field_assignment_747_test.cpp
    # --- fix #747 (R42): readonly auto-property assignable everywhere (§23.1) ---
    sema/readonly_auto_property_747_test.cpp
    # --- fix #747 (R42): private setter not enforced at use site (§13.6) ---
    sema/private_setter_747_test.cpp
    # --- end #747 ---
    sema/expr_identifier_resolution_test.cpp
    sema/expr_operator_types_test.cpp
    sema/member_access_resolution_test.cpp
    sema/parameter_typing_test.cpp
    sema/new_expr_typing_test.cpp
    sema/local_type_inference_test.cpp
    sema/call_return_typing_test.cpp
    sema/definite_assignment_test.cpp
    sema/base_member_lookup_test.cpp
    sema/static_this_check_test.cpp
    sema/readonly_field_test.cpp
    sema/private_visibility_test.cpp
    sema/sealed_class_test.cpp
    sema/case_exhaustiveness_test.cpp
    sema/case_common_type_nullable_747_test.cpp
    sema/multi_base_test.cpp
    sema/abstract_instantiation_test.cpp
    sema/inheritance_cycle_test.cpp
    sema/abstract_impl_test.cpp
    sema/duplicate_member_test.cpp
    sema/hiding_override_test.cpp
    sema/hiding_680_primary_ctor_param_test.cpp
    sema/sealed_member_override_485_test.cpp
    sema/static_class_members_test.cpp
    sema/class_modifier_combos_test.cpp
    sema/bool_condition_test.cpp
    sema/overload_signature_test.cpp
    sema/override_signature_test.cpp
    sema/return_form_test.cpp
    sema/toplevel_visibility_test.cpp
    sema/record_restrictions_test.cpp
    sema/record_base_restrictions_test.cpp
    sema/record_equality_eligibility_test.cpp
    sema/builtin_types_test.cpp
    sema/builtin_exceptions_test.cpp
    sema/string_member_diagnostic_test.cpp
    sema/break_continue_test.cpp
    sema/bare_throw_test.cpp
    sema/primitive_assignability_test.cpp
    sema/assignment_assignability_test.cpp
    sema/mixed_numeric_test.cpp
    sema/missing_operator_diagnostic_test.cpp
    sema/return_assignability_test.cpp
    sema/named_assignability_test.cpp
    sema/accessor_visibility_test.cpp
    sema/call_arity_test.cpp
    sema/call_arg_types_test.cpp
    sema/named_arguments_test.cpp
    sema/overload_resolution_test.cpp
    sema/field_init_restrictions_test.cpp
    sema/primary_ctor_param_init_478_test.cpp
    sema/null_narrowing_test.cpp
    sema/const_type_test.cpp
    sema/local_const_type_test.cpp
    sema/enum_restrictions_test.cpp
    sema/new_expr_args_test.cpp
    sema/bug_483_nested_type_construction_test.cpp
    sema/nested_type_upcast_test.cpp
    sema/method_body_presence_test.cpp
    sema/static_virtual_test.cpp
    sema/async_abstract_test.cpp
    sema/await_typing_test.cpp
    sema/async_lambda_diagnostic_test.cpp
    sema/ctor_chain_test.cpp
    sema/static_ctor_params_test.cpp
    sema/base_visibility_test.cpp
    sema/event_visibility_test.cpp
    sema/protected_internal_visibility_test.cpp
    sema/definite_return_test.cpp
    sema/generic_constraints_test.cpp
    sema/owning_collection_element_761_test.cpp
    sema/generic_return_inference_test.cpp
    sema/generic_static_observe_696_test.cpp
    sema/extern_ffi_type_test.cpp
    sema/entry_point_test.cpp
    sema/flow_narrowing_test.cpp
    sema/unknown_member_test.cpp
    sema/unknown_primitive_method_test.cpp
    sema/method_ref_type_test.cpp
    sema/extension_resolution_test.cpp
    sema/generic_extension_698_test.cpp
    sema/variance_position_test.cpp
    sema/return_in_finally_test.cpp
    sema/untyped_collection_literal_test.cpp
    sema/class_destructuring_pattern_test.cpp
    sema/field_iface_property_489_test.cpp
    sema/typeof_gettype_487_test.cpp
    sema/gettype_chain_511_test.cpp
    sema/dict_iteration_diag_530_test.cpp
    sema/dict_entry_write_reject_624_test.cpp
    sema/lifted_operators_486_sema_test.cpp
    sema/decimal_diagnostics_521_test.cpp
    sema/getoradd_argcheck_542_test.cpp
    sema/icomparable_iequatable_576_test.cpp
    sema/alias_transparency_578_test.cpp
    sema/console_write_arity_591_test.cpp
    sema/user_dispose_method_594_test.cpp
    sema/new_expr_collection_literal_599_test.cpp
    sema/shared_weak_instantiation_603_test.cpp
    sema/serialization_synthesis_550_sema_test.cpp
    sema/serialization_import_gate_631_sema_test.cpp
    sema/serialization_typedriven_703_test.cpp       # #703: type-driven serialization — Shared<T>/Weak<T> component eligibility (§9.8 v0.141)
    # --- #600 contract tests (sendability + Domain.Spawn + channels, §30.14/§30.15) ---
    # These tests are the spec-derived CONTRACT for issue #600 phases 2-3.
    # They are RED against current main: sema has no knowledge of Channel<T>,
    # Domain.Spawn, or the sendable derived type property (§3.6/§30.14).
    # The implementation lane enables them by uncommenting the lines below.
    # DO NOT uncomment until the sendability-check implementation lands.
    #
    sema/send_600_sendability_value_types_test.cpp
    sema/send_600_sendability_class_types_test.cpp
    sema/send_600_spawn_callsite_test.cpp
    sema/send_600_channel_rules_test.cpp
    sema/send_600_runtime_semantics_test.cpp   # #600c (runtime phase) — enabled
    sema/channel_634_uaf_test.cpp              # fix #634: channel core refcounting + send-after-close CCE
    sema/channel_667_valuetype_test.cpp        # fix #667: value-typed payloads, TryReceive/IsClosed after dispose
    sema/channel_640_phantom_members_test.cpp  # fix #640: Channel<T> phantom Send/TrySend/Bogus rejected (§6.9)
    sema/mutex_sendable_647_test.cpp           # #647: Mutex sendable per stdlib §6.2 v0.138; Lock not sendable
    sema/semaphore_sendable_647_test.cpp      # #647: Semaphore sendable per stdlib §6.3 v0.140; SemaphoreToken not sendable
    sema/atomic_647_test.cpp                  # #647: Atomic<T>/AtomicInteger<T> T-restriction, method surface, sendability (stdlib §6.4 v0.140)
    sema/buffer_545_test.cpp                  # issue #545: Buffer<T> T-restriction, Length, Dispose, unknown-method sema
    sema/builtin_674_uniform_unknown_member_test.cpp  # fix #674: uniform no-member diag for Shared/Weak/Mutex/ChannelSender/ArraySpan/BinarySerializer
    sema/shared_sync_sendable_689_test.cpp            # #689: Shared<T> sendable iff T in sync-set (§28.13 v0.140)
    sema/using_var_reassign_830_test.cpp              # fix #830: §28.10 reject reassigning a `using var` local
    sema/move_bare_field_restoration_825_test.cpp     # fix #825: §28.8.1 bare `move _field` must trigger field-restoration check
    sema/nullable_field_narrowing_862_test.cpp        # fix #862: §7.4 reject nullable-field member access — fields are not narrowable
    sema/compound_assign_operator_result_851_test.cpp # fix #851: §25.1 reject compound op whose overload result differs from LHS
    sema/enum_cast_const_range_859_test.cpp           # fix #859: §5.4 reject a constant enum cast outside the underlying range
    sema/bare_drain_discard_832_test.cpp              # fix #832: §21.9 reject bare owning-collection drain discard
    sema/generic_default_new_constraint_864_test.cpp  # fix #864: §17.11 default(T) under where T:class rejected; §14.2 new() at method call site
    # --- end #600 contract ---
    flow/cfg_basic_test.cpp
    flow/cfg_try_catch_test.cpp
    flow/cfg_suspend_test.cpp
    codegen/jit_smoke_test.cpp
    codegen/int_literal_test.cpp
    codegen/int_arithmetic_test.cpp
    codegen/locals_test.cpp
    codegen/conditional_test.cpp
    codegen/conditional_nullable_test.cpp
    codegen/control_flow_test.cpp
    codegen/static_call_test.cpp
    codegen/native_exe_test.cpp
    codegen/async_test.cpp
    codegen/async_618_domain_serial_test.cpp
    codegen/tsan_704_bz_exc_threadlocal_test.cpp
    codegen/async_lambda_488_test.cpp
    codegen/task_combinators_test.cpp
    codegen/instances_test.cpp
    codegen/instance_methods_test.cpp
    codegen/constructors_test.cpp
    codegen/loop_control_test.cpp
    codegen/static_fields_test.cpp
    codegen/disposal_test.cpp
    codegen/free_on_dispose_test.cpp
    codegen/explicit_dispose_reclaim_test.cpp
    codegen/undisposed_leak_test.cpp
    codegen/console_write_leak_test.cpp
    codegen/string_refcount_test.cpp
    codegen/escaping_string_refcount_test.cpp
    codegen/escaping_closure_refcount_test.cpp
    codegen/argpass_string_probe_test.cpp
    codegen/argpass_string_refcount_test.cpp
    codegen/operator_string_refcount_test.cpp
    codegen/encoding_test.cpp
    codegen/closure_refcount_test.cpp
    codegen/env_disposer_672_test.cpp
    codegen/capture_retain_429_test.cpp
    codegen/boxed_cell_free_429_test.cpp
    codegen/closure_call_string_return_429_test.cpp
    codegen/linq_refcount_test.cpp
    codegen/box_refcount_test.cpp
    codegen/escaping_box_refcount_test.cpp
    codegen/owned_field_disposal_test.cpp
    codegen/generic_owned_field_disposal_test.cpp
    codegen/owned_collection_disposal_test.cpp
    codegen/owned_collection_dispose_order_598_test.cpp
    codegen/owned_collection_iface_619_test.cpp
    codegen/remove_equals_test.cpp
    codegen/contains_indexof_equals_test.cpp
    codegen/inherited_fields_test.cpp
    codegen/ctor_chaining_test.cpp
    codegen/virtual_dispatch_test.cpp
    codegen/virtual_property_481_test.cpp
    codegen/sealed_abstract_test.cpp
    codegen/abstract_dynamic_dispose_test.cpp
    codegen/type_test_test.cpp
    codegen/value_boxing_test.cpp
    codegen/object_downcast_test.cpp
    codegen/boxing_tostring_test.cpp
    codegen/exceptions_test.cpp
    codegen/exception_dispose_test.cpp
    codegen/exception_disposal_test.cpp
    codegen/uncaught_exit_test.cpp
    codegen/property_exception_test.cpp
    codegen/owned_exception_field_test.cpp
    codegen/checked_arith_test.cpp
    codegen/null_forgiving_test.cpp
    codegen/float_test.cpp
    codegen/enum_test.cpp
    codegen/nullable_enum_test.cpp
    codegen/nullable_record_test.cpp
    codegen/ffi_test.cpp
    codegen/case_expr_test.cpp
    codegen/record_pattern_constants_615_test.cpp
    codegen/var_destructure_577_test.cpp
    codegen/case_expr_nullable_result_573_test.cpp
    codegen/case_untyped_binding_nullable_747_test.cpp
    codegen/case_type_pattern_null_760_test.cpp
    codegen/destructor_field_move_runtime_763_test.cpp
    codegen/selectmany_lazy_inner_758_test.cpp
    codegen/case_enum_prefix_body_772_test.cpp
    codegen/indexer_overload_774_test.cpp
    codegen/owned_buffer_init_778_test.cpp
    codegen/generic_extension_773_test.cpp
    codegen/inline_owned_array_return_781_test.cpp
    codegen/generic_arg_inference_785_test.cpp
    codegen/generic_record_nullable_field_769_test.cpp
    codegen/generic_record_through_method_766_test.cpp
    codegen/generic_property_getter_768_test.cpp
    codegen/specialized_method_record_return_767_test.cpp
    codegen/generic_record_field_force_unwrap_786_test.cpp
    codegen/generic_record_collection_arg_787_test.cpp
    codegen/async_iter_manual_protocol_759_test.cpp
    codegen/async_forgotten_await_777_test.cpp
    codegen/selectmany_range_selector_775_test.cpp
    codegen/checked_cast_reinterpret_753_test.cpp
    codegen/linq_last_single_predicate_789_test.cpp
    codegen/accessor_grammar_691_test.cpp
    codegen/async_get_async_enumerator_757_test.cpp
    codegen/await_for_with_token_765_test.cpp
    codegen/null_guard_dispose_764_test.cpp
    codegen/property_ref_function_value_756_test.cpp
    codegen/await_for_cancellation_790_test.cpp
    codegen/loop_var_capture_nested_if_754_test.cpp
    codegen/interface_indexer_dispatch_730_test.cpp
    codegen/indexer_via_constraint_794_test.cpp
    codegen/inline_array_nullable_store_813_test.cpp
    codegen/heap_collection_literal_assign_820_test.cpp
    codegen/positional_field_iface_property_815_test.cpp
    codegen/mixed_sign_int_signedness_814_test.cpp
    codegen/case_wide_int_836_test.cpp
    codegen/owned_collection_trypop_831_test.cpp
    codegen/lambda_inferred_param_operator_829_test.cpp
    codegen/nested_generic_chained_field_828_test.cpp
    codegen/nullable_primitive_cast_827_test.cpp
    codegen/owned_set_equality_837_test.cpp
    codegen/owned_array_subscription_838_test.cpp
    codegen/nullable_init_widening_842_test.cpp
    codegen/nullable_return_widening_843_test.cpp
    codegen/nullable_lifted_widening_840_test.cpp
    codegen/async_iter_var_frame_width_833_test.cpp
    codegen/int_min_mod_neg_one_844_test.cpp
    codegen/unary_plus_overload_852_test.cpp
    codegen/generic_nullable_value_param_846_test.cpp
    codegen/nullable_record_user_equality_848_test.cpp
    codegen/typed_binding_string_pattern_856_test.cpp
    codegen/nullable_value_format_interp_849_test.cpp
    codegen/string_compound_concat_857_test.cpp
    codegen/float_to_int_truncation_858_test.cpp
    codegen/enum_cast_range_859_test.cpp
    codegen/nested_catch_rethrow_866_test.cpp
    codegen/generic_nullable_peel_850_test.cpp
    codegen/closure_loop_capture_823_test.cpp
    codegen/lazy_static_init_720_test.cpp
    codegen/lazy_static_init_generic_796_test.cpp
    codegen/for_var_capture_leak_793_test.cpp
    codegen/await_for_frame_dispose_792_test.cpp
    codegen/escaping_member_ref_borrow_791_test.cpp
    codegen/move_result_owned_724_test.cpp
    codegen/expr_bodied_async_799_test.cpp
    codegen/async_lambda_infer_block_800_test.cpp
    codegen/case_record_typed_binding_801_test.cpp
    codegen/async_nested_task_return_776_test.cpp
    codegen/lambda_method_call_on_param_747_test.cpp
    codegen/finally_test.cpp
    codegen/long_int_test.cpp
    codegen/short_circuit_test.cpp
    codegen/integration_test.cpp
    codegen/math_convert_test.cpp
    codegen/math_sign_test.cpp
    codegen/unsigned_int_test.cpp
    codegen/compound_assign_test.cpp
    codegen/compound_assign_operator_overload_610_test.cpp
    codegen/char_literal_test.cpp
    codegen/char_escape_527_test.cpp
    codegen/auto_property_test.cpp
    codegen/with_init_test.cpp
    codegen/objinit_usingvar_disposal_test.cpp
    codegen/generic_usingvar_disposal_test.cpp
    codegen/generic_usingvar_move_disposal_572_test.cpp
    codegen/generic_spec_cache_test.cpp
    codegen/interface_dispatch_test.cpp
    codegen/const_member_test.cpp
    codegen/default_expr_test.cpp
    codegen/case_stmt_test.cpp
    codegen/pattern_guard_member_test.cpp
    codegen/coalesce_test.cpp
    codegen/null_forgiving_member_test.cpp
    codegen/throw_expr_test.cpp
    codegen/null_cond_member_test.cpp
    codegen/null_cond_call_test.cpp
    codegen/conditional_member_test.cpp
    codegen/enum_case_test.cpp
    codegen/enum_property_case_574_test.cpp
    # --- fix #740: non-int enum underlying types (§10.3/§5.4) ---
    codegen/enum_underlying_type_740_test.cpp
    # --- end #740 ---
    codegen/expr_result_member_test.cpp
    codegen/conversion_test.cpp
    codegen/numeric_conversion_test.cpp
    codegen/numeric_shift_nan_char_513_test.cpp
    codegen/move_expr_test.cpp
    codegen/move_collection_slot_test.cpp
    codegen/default_args_test.cpp
    codegen/named_args_test.cpp
    codegen/overload_test.cpp
    codegen/ctor_args_test.cpp
    codegen/static_ctor_test.cpp
    codegen/static_init_exception_529_test.cpp
    codegen/static_field_init_order_582_test.cpp
    codegen/operator_test.cpp
    codegen/operator_increment_498_test.cpp
    codegen/block_setter_test.cpp
    codegen/ref_out_params_test.cpp
    codegen/record_basics_test.cpp
    codegen/nested_record_field_test.cpp
    codegen/record_computed_property_test.cpp
    codegen/record_equality_test.cpp
    codegen/value_semantics_test.cpp
    codegen/body_record_equality_test.cpp
    codegen/record_string_equality_test.cpp
    codegen/inline_array_test.cpp
    codegen/inline_array_equality_test.cpp
    codegen/inline_owned_array_disposal_747_test.cpp
    codegen/generic_base_this_base_call_747_test.cpp
    codegen/async_iter_frame_spill_747_test.cpp
    codegen/sync_iter_frame_spill_747_test.cpp
    codegen/event_ievent_var_747_test.cpp
    codegen/string_test.cpp
    codegen/string_length_codepoints_528_test.cpp
    codegen/string_methods_codepoints_535_test.cpp
    codegen/string_compare_test.cpp
    codegen/nullable_string_compare_test.cpp
    codegen/string_tostring_test.cpp
    codegen/interpolated_string_test.cpp
    codegen/string_indexer_test.cpp
    codegen/string_methods_test.cpp
    codegen/stringbuilder_methods_test.cpp
    codegen/string_for_in_test.cpp
    codegen/range_for_test.cpp
    codegen/range_in_var_499_test.cpp
    codegen/lambda_test.cpp
    codegen/closure_capture_test.cpp
    codegen/generic_test.cpp
    codegen/generic_mono_test.cpp
    codegen/generic_base_field_585_test.cpp
    codegen/generic_second_typeparam_597_test.cpp
    codegen/generic_nullable_typeparam_604_605_test.cpp
    codegen/generic_nullable_field_r33_test.cpp
    codegen/generic_reversed_params_616_test.cpp
    codegen/generic_concrete_base_617_test.cpp
    codegen/generic_record_field_590_test.cpp
    codegen/generic_return_inference_test.cpp
    codegen/generic_collection_return_test.cpp
    codegen/collection_test.cpp
    codegen/record_key_test.cpp
    codegen/collection_keys_test.cpp
    codegen/set_algebra_complete_test.cpp
    codegen/linq_test.cpp
    codegen/linq_completeness_test.cpp
    codegen/linq_gaps_test.cpp
    codegen/linq_throw_469_test.cpp
    codegen/toarray_inference_test.cpp
    codegen/array_reverse_chain_466_test.cpp
    codegen/event_test.cpp
    codegen/event_477_test.cpp
    codegen/event_479_test.cpp
    codegen/event_leak_480_test.cpp
    codegen/cancellation_token_test.cpp
    codegen/mutex_lock_647_test.cpp             # §6.2 Mutex + Lock, issue #647
    codegen/semaphore_647_test.cpp              # §6.3 Semaphore + SemaphoreToken, issue #647
    codegen/atomic_647_test.cpp                 # §6.4 Atomic<T>/AtomicInteger<T>, issue #647
    codegen/buffer_545_test.cpp                 # issue #545: Buffer<T> groundwork — value-element compiler primitive
    codegen/datetime_timespan_test.cpp
    codegen/stdlib_wave1_648_649_652_test.cpp
    codegen/stdlib_wave2_test.cpp
    codegen/stdlib_654_bool_bitconv_tochararray_test.cpp  # issue #654: bool.TryParse §9.4, BitConverter §2.12, string.ToCharArray §9.6
    codegen/timespan_encoding_525_526_test.cpp
    codegen/decimal_521_test.cpp
    codegen/decimal_nullable_663_test.cpp
    codegen/guid_test.cpp
    codegen/owned_collection_ops_test.cpp
    codegen/linked_list_test.cpp
    codegen/bug_453_454_455_test.cpp
    codegen/owned_dict_disposal_test.cpp
    codegen/dict_clear_test.cpp
    codegen/dict_get_or_add_test.cpp
    codegen/collection_indexer_test.cpp
    codegen/interface_indexer_test.cpp
    codegen/user_indexer_record_test.cpp
    codegen/dict_record_value_indexer_test.cpp
    codegen/dict_record_value_leak_test.cpp
    codegen/is_narrow_receiver_test.cpp
    codegen/null_narrow_member_test.cpp
    codegen/generic_record_return_test.cpp
    codegen/generic_record_field_460_test.cpp
    codegen/generic_record_with_eq_nullable_462_test.cpp
    codegen/generic_record_this_with_method_test.cpp
    codegen/iterator_native_test.cpp
    codegen/extension_method_test.cpp
    codegen/generic_extension_ir_699_test.cpp
    codegen/bug_457_458_test.cpp
    codegen/bug_464_test.cpp
    codegen/bug_465_test.cpp
    codegen/bug_475_fp_predicates_test.cpp
    codegen/bug_474_string_slice_test.cpp
    codegen/async_451_test.cpp
    codegen/primary_ctor_param_init_478_test.cpp
    codegen/static_property_setter_476_test.cpp
    codegen/bug_483_nested_type_test.cpp
    codegen/generic_constraint_dispatch_484_test.cpp
    codegen/record_generic_constraint_test.cpp
    codegen/nested_enum_cast_482_test.cpp
    codegen/generic_constraint_property_dispatch_test.cpp
    codegen/constraint_dispatch_gaps_test.cpp
    codegen/array_record_element_test.cpp
    codegen/owned_collection_remove_test.cpp
    codegen/lambda_codegen_fixes_test.cpp
    codegen/iife_507_test.cpp
    codegen/indexer_compound_assign_test.cpp
    codegen/array_indexer_compound_508_test.cpp
    codegen/nameof_charrepeat_test.cpp
    codegen/blockform_property_itable_test.cpp
    codegen/ref_return_generic_490_test.cpp
    codegen/async_nullable_return_493_test.cpp
    codegen/itable_bool_return_494_test.cpp
    codegen/stringbuilder_capacity_497_test.cpp
    codegen/split_result_type_500_test.cpp
    codegen/ffi_name_collision_496_test.cpp
    codegen/driver_ir_fail_exit_test.cpp
    codegen/generic_mono_value_504_505_test.cpp
    codegen/property_accessor_types_510_test.cpp
    codegen/explicit_typeargs_509_test.cpp
    codegen/primitive_tostring_ieee_518_519_test.cpp
    codegen/generic_value_method_515_516_test.cpp
    codegen/collections_r9_522_523_524_test.cpp
    codegen/array_reversed_sorted_472_test.cpp
    codegen/sorted_comparer_record_533_test.cpp
    codegen/generic_static_520_test.cpp
    codegen/generic_static_state_536_test.cpp
    codegen/generic_static_local_direct_697_test.cpp
    codegen/generic_static_cross_named_701_test.cpp
    codegen/generic_static_nonconst_init_709_test.cpp
    codegen/generic_static_sameclass_715_test.cpp
    codegen/lifted_operators_486_test.cpp
    codegen/dict_literal_534_test.cpp
    codegen/stringbuilder_ctor_531_test.cpp
    codegen/dict_values_iter_537_test.cpp
    codegen/dict_literal_contexts_538_test.cpp
    codegen/dict_nullable_value_539_test.cpp
    codegen/dict_nullable_read_544_test.cpp
    codegen/lambda_nullable_return_541_test.cpp
    codegen/lambda_nullable_cases_543_test.cpp
    codegen/closure_call_nullable_param_557_test.cpp
    codegen/recursive_lambda_579_test.cpp
    codegen/record_named_args_540_test.cpp
    codegen/in_operator_collection_551_test.cpp
    codegen/in_operator_string_llist_558_560_test.cpp
    codegen/llist_contains_eq_562_test.cpp
    codegen/llist_remove_eq_565_test.cpp
    codegen/llist_find_eq_566_test.cpp
    codegen/dict_keys_values_view_556_test.cpp
    codegen/dict_record_values_snapshot_563_test.cpp
    codegen/dict_decode_570_571_test.cpp
    codegen/set_literal_559_test.cpp
    codegen/generic_method_bare_typeargs_561_test.cpp
    codegen/generic_array_index_type_564_test.cpp
    codegen/generic_coll_keyhash_567_test.cpp
    codegen/generic_nullable_typeparam_568_569_test.cpp
    codegen/array_span_549_test.cpp
    codegen/record_with_test.cpp
    codegen/record_this_with_575_test.cpp
    codegen/writeline_nullable_581_583_test.cpp
    codegen/primitive_compareto_586_test.cpp
    codegen/virtual_generic_override_580_test.cpp
    codegen/indexer_incdec_587_test.cpp
    codegen/catch_expr_body_588_test.cpp
    codegen/console_write_unicode_592_test.cpp
    codegen/multitype_catch_ncb_593_test.cpp
    codegen/user_exception_message_595_test.cpp
    codegen/user_exception_isa_596_test.cpp
    codegen/checked_disposal_601_test.cpp
    codegen/checked_disposal_621_test.cpp
    codegen/checked_disposal_627_test.cpp
    codegen/shared_weak_603_test.cpp
    codegen/shared_weak_nullable_620_test.cpp
    codegen/shared_sync_689_contention_test.cpp       # #689: atomic CB + Shared<Mutex> cross-domain contention (§28.13 v0.140)
    codegen/user_exception_tostring_608_test.cpp
    codegen/builtin_collection_incdec_613_test.cpp
    codegen/builtin_collection_compound_overload_626_test.cpp
    codegen/dict_entry_iter_548_test.cpp
    codegen/dict_insertion_order_622_test.cpp
    codegen/async_iter_try_finally_554_test.cpp
    codegen/async_iter_await_finally_655_test.cpp  # fix #655: await-in-async-iterator-finally rejected (§30.7 v0.137)
    codegen/iter_finally_dispose_623_test.cpp
    codegen/sync_iter_yield_restrictions_r33_test.cpp  # R33: §29.6 yield-placement for sync iterators
    codegen/scrutineeless_case_r34_test.cpp  # R34: scrutinee-less case → clean diagnostic (not silent garbage)
    codegen/iterator_exception_630_test.cpp
    codegen/codegen_unimplemented_hard_error_628_test.cpp
    codegen/owned_dict_drain_628_test.cpp
    codegen/owned_drain_639_test.cpp
    codegen/owned_collection_accessors_test.cpp
    codegen/user_exception_unqualified_msg_625_test.cpp
    codegen/serialization_synthesis_550_test.cpp
    codegen/main_args_633_test.cpp
    codegen/serialization_edges_635_637_638_test.cpp
    codegen/ownedbuffer_span_636_test.cpp
    codegen/file_io_644_test.cpp
    codegen/diagnostics_646_test.cpp
    codegen/decimal_integration_664_666_test.cpp
    # --- fix #686: nullable-decimal LINQ aggregates ---
    codegen/decimal_nullable_686_test.cpp
    # --- end #686 ---
    # --- #550 tier-2 contract tests (class trees, §9.8) ---
    # Spec-derived CONTRACT for #550 tier 2; 23/25 red until the implementation
    # lands. Enable with the tier-2 implementation merge (pattern: #600 contract).
    codegen/serialization_tier2_classes_test.cpp
    # --- end #550 tier-2 contract ---
    # --- #550 tier-3 contract tests (serialized-graph back-edges, §9.8) ---
    codegen/serialization_tier3_graph_test.cpp
    # --- end #550 tier-3 contract ---
    # --- #550 tier-4 contract tests (polymorphic registry, §9.8) ---
    codegen/serialization_tier4_poly_test.cpp
    # --- end #550 tier-4 contract ---
    # --- #668/#669 fix tests (missing-required-field raise; AOT null-nullable-class) ---
    codegen/serialization_668_669_test.cpp
    # --- end #668/#669 ---
    # --- #688 fix tests (SerializationException position info — byte offset / line/column) ---
    codegen/serialization_688_posinfo_test.cpp
    # --- end #688 ---
    # --- #550 JSON format tier (§2.15 JsonSerializer/JsonDeserializer) ---
    codegen/json_550_test.cpp
    # --- end #550 JSON ---
    # --- fix #687: JsonOptions registered + wired (§2.15) ---
    codegen/json_options_687_test.cpp
    # --- end #687 ---
    # --- fix #685/#679/#682: struct-as-scalar lowering (decimal Set/Dict, field Mutex, range slice) ---
    codegen/struct_scalar_685_679_682_test.cpp
    # --- end #685/#679/#682 ---
    # --- fix #690: arr[range] typed as Array<T> per §15.6 ---
    codegen/array_slice_690_test.cpp
    # --- end #690 ---
    # --- fix #741: Array<T>.Capacity + ctor capacity reservation (§4.4/§4.5) ---
    codegen/array_capacity_741_test.cpp
    # --- end #741 ---
    # --- fix #743: static auto-property backing global (§23.1) ---
    codegen/static_auto_property_743_test.cpp
    # --- end #743 ---
    # --- fix #746: case typed-binding unbox over Object? (R41 A, §19.3/§8.6) ---
    codegen/case_typed_binding_unbox_746_test.cpp
    # --- fix #746: implicit numeric conversions (R41 H/I, §5.1) ---
    codegen/implicit_numeric_conv_746_test.cpp
    # --- fix #746: OwnedStack/Queue Pop/Dequeue disposal (R41 F, §4.11) ---
    codegen/owned_collection_pop_dispose_746_test.cpp
    # --- fix #746: AsSpan(Range) endpoint (R41 J, §4.14) ---
    codegen/asspan_range_746_test.cpp
    # --- end #746 ---
    # --- fix #747 (R42 #0): static field via instance receiver crashed codegen (§22.12) ---
    codegen/static_field_via_instance_747_test.cpp
    # --- fix #747 (R42 IR_FAIL): `return` in a sync iterator → §29.5 diagnostic ---
    codegen/iterator_return_747_test.cpp
    # --- fix #747 (R42 WRONG_VALUE): char.MinValue/MaxValue emit no store ---
    codegen/char_minmax_747_test.cpp
    # --- fix #747 (R42 WRONG_VALUE): unary negation of unsigned must overflow (§6.1) ---
    codegen/unsigned_negate_747_test.cpp
    # --- fix #747 (R42): int.Parse(s,radix) dropped + decimal.Parse keyword crash ---
    codegen/parse_overloads_747_test.cpp
    # --- end #747 ---
    # --- fix #677: record method case-expr with `_ => this` arm (PHI type mismatch) ---
    # --- #744 stage 1: dedicated `when` condition-dispatch expression (§19) ---
    codegen/when_expr_744_test.cpp
    codegen/record_method_case_expr_677_test.cpp
    # --- fix #747 (R42): this[i] user-indexer get/set/compound no-op (§22.10) ---
    codegen/this_indexer_747_test.cpp
    # --- end #677 ---
    # --- fix #678: lambda void return type — inferred-param/untyped-var void body ---
    codegen/lambda_void_return_678_test.cpp
    # --- end #678 ---

    module/module_metadata_test.cpp
    module/module_metadata_builder_test.cpp
    module/cross_module_import_test.cpp
    module/cross_module_exception_test.cpp
    module/cross_module_iterator_test.cpp
    module/xmod_property_503_test.cpp
    module/xmod_record_nested_501_502_test.cpp
    module/xmod_string_refcount_694_test.cpp
    module/xmod_owned_class_return_707_test.cpp
    module/xmod_closure_return_712_test.cpp
    module/xmod_enum_method_735_test.cpp  # #735: imported enum instance method i32 receiver ABI
    module/xmod_static_init_798_test.cpp  # #798: imported type's static init runs cross-module

    runtime/event_subscription_test.cpp
    runtime/concurrent_dispose_706_test.cpp
    runtime/atomic_cancelstate_716_test.cpp
    runtime/current_domain_713_test.cpp

    ownership/linear_owned_test.cpp
    ownership/read_after_move_test.cpp
    ownership/branch_joins_test.cpp
    ownership/branch_disposal_test.cpp
    ownership/owned_params_test.cpp
    ownership/borrow_escape_test.cpp
    ownership/move_on_fields_test.cpp
    ownership/exception_path_test.cpp
    ownership/exception_discharge_test.cpp
    ownership/owned_field_da_test.cpp
    ownership/owned_return_test.cpp
    ownership/call_arg_move_test.cpp
    ownership/assignment_consumes_test.cpp
    ownership/cfg_dataflow_test.cpp
    ownership/loop_fixpoint_test.cpp
    ownership/escape_closure_record_test.cpp
    ownership/escape_deeper_test.cpp
    ownership/catch_escape_test.cpp
    ownership/out_owned_discharge_test.cpp
    ownership/owned_sink_test.cpp
    ownership/owned_field_sink_test.cpp
    ownership/owned_sink_sources_test.cpp
    ownership/inline_owned_array_slot_sink_780_test.cpp
    ownership/using_var_return_762_test.cpp
    ownership/destructor_field_move_763_test.cpp
    ownership/owned_field_assign_test.cpp
    ownership/catch_owned_sink_test.cpp
    ownership/throw_owned_arg_test.cpp
    ownership/catch_borrow_escaping_via_ctor_test.cpp
    ownership/async_ownership_test.cpp
    ownership/move_interface_slot_test.cpp
    ownership/value_builtin_ownership_test.cpp
    ownership/use_after_dispose_warn_601_test.cpp
    ownership/channel_spawn_sink_692_693_test.cpp
    ownership/nested_new_owned_arg_835_test.cpp)

add_executable(bezel_tests ${BEZEL_TEST_SOURCES})
target_include_directories(bezel_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bezel_tests PRIVATE bezel_core bezel_warnings)
# Force all bezel_runtime object files into bezel_tests, then export-dynamic
# so the JIT (DynamicLibrarySearchGenerator::GetForCurrentProcess) can resolve
# bz_array_*, bz_dict_*, etc. from the process symbol table at JIT run time.
target_link_options(bezel_tests PRIVATE
    -Wl,--whole-archive $<TARGET_FILE:bezel_runtime> -Wl,--no-whole-archive
    -Wl,--export-dynamic)

add_test(NAME bezel_tests COMMAND bezel_tests)

# Fast full-suite run: the single bezel_tests binary honours
# BEZEL_SHARD_COUNT / BEZEL_SHARD_INDEX (test_main.cpp) to run a disjoint subset,
# so run_parallel.sh launches one process per core and runs the whole suite in a
# fraction of the serial wall-clock (≈186s serial → ≈25s on 16 shards) while
# still executing every test. This is the blessed way to run the full suite for
# a merge gate:  cmake --build build --target check
add_custom_target(check
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel.sh
    DEPENDS bezel_tests
    USES_TERMINAL
    COMMENT "Running full test suite in parallel shards (run_parallel.sh)")
