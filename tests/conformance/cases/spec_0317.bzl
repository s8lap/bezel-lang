Type            ::= TypeNoNullable "?"?
TypeNoNullable  ::= QualifiedName GenericArgs?
                  | PrimitiveType
                  | InlineArrayType
                  | FunctionType
                  | "(" FunctionType ")"       /* parenthesized function type,
                                                  required before the `?`
                                                  nullable marker (§27.2):
                                                  `((int) -> bool)?`. In `-> T?`
                                                  the `?` binds to the return
                                                  type, not the function type. */
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
ConstIntExpr    ::= Expression           /* a §17.14 constant expression of
                       integer type; see §15.1 "Size" */
GenericArgs     ::= "<" GenericArg ("," GenericArg)* ","? ">"
GenericArg      ::= Type
                              /* `owned` is not admitted on
                                 generic type arguments; the owned-bearing
                                 collection and task types are separate named
                                 classes (`OwningArray<T>`, `OwningTask<T>`, etc.).
                                 See §14.4. */
FunctionType    ::= "(" (FuncParamType ("," FuncParamType)* ","?)? ")" "->" "owned"? Type
                              /* The optional `owned` on the return marks
                                 a function type whose result transfers
                                 ownership to the caller. See §28.1. The
                                 `owned` is part of the function type's
                                 identity: `() -> Connection` and
                                 `() -> owned Connection` are distinct types. */
FuncParamType   ::= FuncParamMods? (Identifier ":")? Type
FuncParamMods   ::= "ref" | "scoped"
                  | "out" "owned"?
                  | "owned"
                              /* Same set as the method-declaration ParamModifiers
                                 (§A.4); function types and method declarations
                                 share modifier syntax exactly. The `out owned`
                                 combination represents an out-parameter that
                                 produces an owned value. */
IntegerType     ::= "byte" | "sbyte" | "short" | "ushort"
                  | "int" | "uint" | "long" | "ulong"
PrimitiveType   ::= IntegerType | "float" | "double" | "decimal"
                  | "bool" | "char" | "string"
                              /* The primitive KEYWORD spellings are tokens, not
                                 identifiers (§1.4, §3.4), so `var x: int` and
                                 `int.Parse(s)` need this production; the
                                 PascalCase aliases (`Int32`, `String`, …) arrive
                                 through QualifiedName as ordinary identifiers. */
