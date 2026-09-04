ClassDecl       ::= Visibility? Modifier* "class" Identifier GenericParams?
                    PrimaryCtorParams?
                    Inheritance?
                    GenericConstraints*
                    ClassBody
PolymorphicMember ::= "polymorphic" "(" PolyCase ("," PolyCase)* ","? ")" ";"
PolyCase ::= StringLiteral ":" QualifiedName
                              /* A `polymorphic` registry is a class-body member
                                 (at most one per class; takes no visibility).
                                 Each PolyCase maps a wire discriminator string
                                 to a permitted concrete derived type. The set is
                                 closed and explicitly registered: there is no
                                 reflective discovery of subtypes (rationale
                                 §5.18, §5.22). The discriminator must be a
                                 non-empty literal and unique within the
                                 registry; each QualifiedName must name a type
                                 assignable to the declaring class and itself
                                 type-driven serialization-eligible (declares
                                 ISerializable/IDeserializable). The declaring
                                 base class may be concrete (it registers its own
                                 discriminator) or abstract (no self case).
                                 `polymorphic` is a contextual word here, not a
                                 reserved keyword. A class enters serialization
                                 synthesis by declaring ISerializable/IDeserializable;
                                 the serialized shape is read from its field
                                 types (§9.8), with no class-header clause. */
                              /* The grammar admits modifier combinations the
                                 type system rejects: `static class C(args)`
                                 parses (Modifier* allows `static`,
                                 PrimaryCtorParams? is optional and present)
                                 but is rejected semantically — static classes
                                 cannot be instantiated and so cannot carry
                                 primary-ctor parameters (§11.7). */
ClassBody       ::= Member* "end" Identifier? ";"
Modifier        ::= "abstract" | "closed" | "sealed" | "static"
                              /* Modifier* permits any combination; the
                                 semantically valid combinations are described
                                 in §11.7. `closed` (closed abstract class)
                                 combines with no other class-shape modifier:
                                 `closed abstract`, `closed sealed`, and
                                 `closed static` are rejected, as is the
                                 pre-v0.242 spelling `sealed abstract` (the
                                 diagnostic names `closed`); other combinations
                                 with `static` are rejected. */
PrimaryCtorParams ::= "(" ParamList? ")"
Inheritance     ::= ":" InheritItem ("," InheritItem)* ","?
InheritItem     ::= Type ("(" ArgList? ")")?
                              /* The grammar accepts the optional argument list
                                 on every InheritItem; semantically, the
                                 argument list is permitted only on the base
                                 class (the first item), not on interfaces. */

InterfaceDecl   ::= Visibility? "interface" Identifier GenericParams?
                    Inheritance?
                    GenericConstraints*
                    InterfaceMember* "end" Identifier? ";"

RecordDecl      ::= Visibility? "record" Identifier GenericParams?
                    PositionalFields?
                    Inheritance?
                    GenericConstraints*
                    ( ";" | RecordBody )
PositionalFields ::= "(" PositionalField ("," PositionalField)* ","? ")"
PositionalField ::= Identifier ":" Type SerializedClause? EqualityClause?
EqualityClause  ::= "equality" "skip"
                              /* Per-member exclusion from auto-equality
                                 (§9.2 "The `equality skip` clause"),
                                 mirroring SerializedClause: the marked
                                 storage component is removed from the
                                 §9.2 membership computation and cannot
                                 disqualify the record. `equality` is
                                 contextual (B.2), and within the clause
                                 `skip` is likewise contextual; only the
                                 skip form exists (no rename, no optional).
                                 Attached at PositionalField, the
                                 auto-property alternative of PropertyDecl
                                 (A.4), FieldDecl (A.4), and CaseField —
                                 in the fixed order SerializedClause?
                                 EqualityClause? where both slots exist
                                 (an arbitrary but stated order, keeping
                                 the grammar LL; each clause at most once).
                                 NOT on EnumConstant (an enum value is not
                                 a component) and NOT on ChoiceCase (the
                                 case is the discriminant); on computed
                                 properties, methods, and statics the
                                 clause is a compile error (§9.2's
                                 inert-annotation rule). */
SerializedClause ::= "serialized" ( "skip" | SerializedName )
SerializedName   ::= StringLiteral? "optional"?   /* at least one of the two
                                                     must be present */
                              /* Compiler-recognized field-name customization
                                 for serialization synthesis (§9.8). NOT an
                                 attribute system: a closed vocabulary, no
                                 user extensibility, no runtime introspection.
                                 `serialized "name"` renames; `serialized
                                 optional` makes the field tolerant
                                 (omit-if-null on write, default-if-missing on
                                 read); `serialized "name" optional` combines
                                 the two; `serialized skip` excludes the field
                                 and is mutually exclusive with a name or
                                 `optional`. `skip` and `optional` are
                                 contextual words here, not reserved keywords;
                                 the `SerializedName` form requires at least
                                 one of the string or `optional` (a bare
                                 `serialized` is a syntax error). */
RecordBody      ::= Member* "end" Identifier? ";"

EnumDecl        ::= Visibility? "enum" Identifier EnumInheritance?
                    EnumConstants
                    EnumMember* "end" Identifier? ";"
EnumInheritance ::= ":" IntegerType ("," InheritItem)* ","?
                  | ":" InheritItem ("," InheritItem)* ","?
                              /* The first form is `enum E: int, IComparable<E>` —
                                 explicit underlying type followed by interfaces.
                                 The second form is `enum E: IComparable<E>` —
                                 default underlying type (int), interfaces only.
                                 The grammar disambiguates by whether the first
                                 token after `:` is an IntegerType keyword. */
EnumConstants   ::= EnumConstant ("," EnumConstant)* ","? ";"
EnumConstant    ::= Identifier (":=" ConstExpr)? SerializedClause?

ChoiceDecl      ::= Visibility? "choice" Identifier GenericParams?
                    Inheritance?
                    GenericConstraints*
                    ChoiceCases
                    ChoiceMember* "end" Identifier? ";"
ChoiceCases     ::= ChoiceCase ("," ChoiceCase)* ","? ";"
ChoiceCase      ::= Identifier CasePayload? SerializedClause?
                              /* SerializedClause on a case admits the rename
                                 form only; `skip` and `optional` are rejected
                                 semantically, as on enum constants (§9.8). */
CasePayload     ::= "(" CaseField ("," CaseField)* ","? ")"
CaseField       ::= "owned"? Identifier ":" Type SerializedClause? EqualityClause?
                              /* The payload field form is PositionalField plus
                                 the optional leading `owned` marker; the
                                 marker's admissibility follows §28.1's type
                                 requirement, including the bare-type-parameter
                                 erasure. Inheritance names interfaces only
                                 (as for records). §35.1. */
ChoiceMember    ::= EnumMember
                              /* a choice body admits exactly the enum
                                 member set (§35.2). */
                              /* `serialized "name"` on an enum constant
                                 customizes that member's serialized name
                                 (§9.8). `serialized skip` and `serialized
                                 optional` are not admitted on enum constants
                                 — an enum value is a single member, not a
                                 field that can be omitted or defaulted; using
                                 them here is a compile error. */

ExtensionDecl   ::= Visibility? "extension" Type GenericConstraints*
                    ExtensionMember* "end" "extension"? ";"

ExternBlock     ::= "extern" StringLiteral ("convention" StringLiteral)?
                    ExternDecl+ "end" "extern"? ";"
ExternDecl      ::= Visibility? "method" Identifier "(" ExternParamList? ")"
                    (":" "owned"? Type)?
                    (":=" "symbol" StringLiteral VariadicClause?)? ";"
ExternParamList ::= ExternParam ("," ExternParam)* ","?
ExternParam     ::= Param | "env"
VariadicClause  ::= "variadic" "(" IntLiteral ")"
                              /* `owned` on the return marks ownership transfer
                                 per §32.6's contract (§28.1: owned returns are
                                 declared). `symbol`, `variadic`, and `env` are
                                 contextual (Appendix B.2). In ExternParam the
                                 parser distinguishes by two-token lookahead:
                                 `env` followed by `:` is an ordinary Param
                                 named `env`; a bare `env` is the user-data
                                 slot marker (§32.8). `env` also appears as a
                                 parameter-type atom inside a function-typed
                                 extern parameter (§32.8); the type grammar
                                 admits it there as an ordinary identifier and
                                 §32.8's pairing rules give it meaning — the
                                 over-admit-and-reject-semantically pattern.
                                 Param modifiers: `ref`/`out` admitted per
                                 §32.5's boundary rules; `owned`, `out owned`,
                                 and `scoped` rejected semantically. */
