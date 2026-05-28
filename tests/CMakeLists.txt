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
    parser/case_and_patterns_test.cpp
    parser/case_stmt_test.cpp
    parser/nested_types_test.cpp
    parser/serialized_clause_test.cpp)

add_executable(bezel_tests ${BEZEL_TEST_SOURCES})
target_include_directories(bezel_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bezel_tests PRIVATE bezel_core bezel_warnings)

add_test(NAME bezel_tests COMMAND bezel_tests)
