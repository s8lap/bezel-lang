SourceFile      ::= ModuleDecl Import* TopDecl*
ModuleDecl      ::= "module" QualifiedName ";"
Import          ::= "import" QualifiedName ("as" Identifier)? ";"
                              /* alias: a file-scoped qualifier for the
                                 imported module (§2.2). */
QualifiedName   ::= Identifier ("." Identifier)*

TopDecl         ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl
                  | ChoiceDecl
                  | ExtensionDecl | TypeAliasDecl | ExternBlock
TypeAliasDecl   ::= "type" Identifier GenericParams? ":=" Type
                              /* The alias binds with `:=`, like every other
                                 binding. No Visibility alternative: a module-level alias
                                 is always module-internal (§27.8). */
                    GenericConstraints* ";"
