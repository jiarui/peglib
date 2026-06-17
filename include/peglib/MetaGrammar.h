#pragma once
#include <memory>
#include <span>
#include <string>

#include "peglib/Grammar.h"
#include "peglib/PegAst.h"
#include "peglib/Rule.h"

namespace peg
{

// Placeholder — will be rewritten with tree-based actions in W2.
inline const Grammar<PegParseCtx>& meta_grammar()
{
    static Grammar<PegParseCtx> g;
    return g;
}

} // namespace peg
