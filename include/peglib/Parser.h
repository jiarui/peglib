#pragma once

// Parser.h is an umbrella header that includes all parser expression types.
// Individual headers can be used directly for finer-grained dependencies:
//   ParserFwd.h    — ScopeGuard, ParsingExprInterface, ParsingExpr, symbolConsumable
//   Terminals.h    — TerminalExpr, TerminalSeqExpr, EmptyExpr
//   Combinators.h  — SequenceExpr, AlternationExpr, Repetition (+ variants),
//                    NotExpr, AndExpr, CutExpr
//   NonTerminal.h  — NonTerminal (internal node), Rule (bare-pointer handle)

#include "peglib/Combinators.h"
#include "peglib/NonTerminal.h"
#include "peglib/ParserFwd.h"
#include "peglib/Terminals.h"
