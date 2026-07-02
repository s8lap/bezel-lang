Type            ::= TypeNoNullable "?"?
TypeNoNullable  ::= QualifiedName GenericArgs?
                  | InlineArrayType
                  | FunctionType
                  | "(" ")"                    /* unit type */
InlineArrayType ::= "[" ConstIntExpr "owned"? "]" Type
                              /* Inline array. Size must be a compile-time
                                 integer constant > 0. The optional `owned`
                                 marker (between size and closing bracket)
                                 marks slot ownership; the array becomes
                                 a move-only value type. See §15.1, §15.3.
                                 The inner Type is parsed greedily — `[3]int?`
                                 means an inline array of nullable int, not
                                 a nullable inline array. Nullable inline-array
                                 types are not provided in this revision. */
ConstIntExpr    ::= /* a compile-time integer expression — integer literals,
                       const declarations referring to integer values,
                       and arithmetic on these. See §15.1 "Size". */
GenericArgs     ::= "<" GenericArg ("," GenericArg)* ">"
GenericArg      ::= Type
                              /* In v0.97 and later, `owned` is not admitted on
                                 generic type arguments; the owned-bearing
                                 collection and task types are separate named
                                 classes (`OwningArray<T>`, `OwningTask<T>`, etc.).
                                 See §14.4. */
FunctionType    ::= "(" (FuncParamType ("," FuncParamType)*)? ")" "->" "owned"? Type
                              /* The optional `owned` on the return marks
                                 a function type whose result transfers
                                 ownership to the caller. See §28.1. The
                                 `owned` is part of the function type's
                                 identity: `() -> Connection` and
                                 `() -> owned Connection` are distinct types. */
FuncParamType   ::= FuncParamMods? (Identifier ":")? Type
FuncParamMods   ::= "ref"
                  | "out" "owned"?
                  | "owned"
                              /* Same set as the method-declaration ParamModifiers
                                 (§A.4); function types and method declarations
                                 share modifier syntax exactly. The `out owned`
                                 combination represents an out-parameter that
                                 produces an owned value. */
IntegerType     ::= "byte" | "sbyte" | "short" | "ushort"
                  | "int" | "uint" | "long" | "ulong"
