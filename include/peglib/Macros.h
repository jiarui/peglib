#pragma once

#include <string>

namespace peg
{

// Helper: set the rule name to the stringified identifier and return the rule.
// Used by the PEG_RULE macro; exposed as a free function for manual use.
template<typename Rule>
void peg_rule_name(Rule& rule, std::string name)
{
    rule.set_name(std::move(name));
}

template<typename Rule>
void peg_rule_label(Rule& rule, std::string label)
{
    rule.set_label(std::move(label));
}

} // namespace peg

// ---------------------------------------------------------------------------
// PEG_RULE(Context, NAME, EXPR)
//   Declares a NonTerminal named NAME, assigns EXPR to it, and sets its
//   rule name to the stringified NAME for error reporting.
//
//   PEG_RULE(MyContext, numeral, '0'-'9' >> +('0'-'9'));
//   // expands to:
//   //   MyContext::Rule numeral = ('0'-'9' >> +('0'-'9'));
//   //   numeral.set_name("numeral");
//
// PEG_RULE_LABELED(Context, NAME, LABEL, EXPR)
//   Same as PEG_RULE but also sets a human-readable label (takes priority
//   over name in error messages).
//
//   PEG_RULE_LABELED(MyContext, Numeral, "a number", '0'-'9' >> +('0'-'9'));
//
// PEG_RULE_DEF(Context, NAME, EXPR)
//   For recursive rules — where EXPR references NAME itself. Uses
//   default-construct + assign (forward-declare pattern) instead of
//   copy-initialization. This is required because:
//
//     Rule<> r = r >> 'b' | 'a';   // CRASHES: r.m_impl is uninitialized
//     Rule<> r; r = r >> 'b' | 'a'; // OK: r.m_impl valid before assignment
//
//   PEG_RULE_DEF(MyContext, expr, expr >> '+' >> expr | term);
//   // expands to:
//   //   MyContext::Rule expr;
//   //   expr = (expr >> '+' >> expr | term);
//   //   expr.set_name("expr");
//
// PEG_RULE_DEF_LABELED(Context, NAME, LABEL, EXPR)
//   Same as PEG_RULE_DEF but also sets a human-readable label.
//
// PEG_RULE_RECURSIVE(Context, NAME, EXPR)
//   Alias for PEG_RULE_DEF. Use whichever reads better in context.
// ---------------------------------------------------------------------------

#define PEG_RULE(CTXT, NAME, EXPR)                                                                 \
    typename CTXT::Rule NAME = (EXPR);                                                             \
    ::peg::peg_rule_name(NAME, #NAME)

#define PEG_RULE_LABELED(CTXT, NAME, LABEL, EXPR)                                                  \
    typename CTXT::Rule NAME = (EXPR);                                                             \
    ::peg::peg_rule_name(NAME, #NAME);                                                             \
    ::peg::peg_rule_label(NAME, LABEL)

#define PEG_RULE_DEF(CTXT, NAME, EXPR)                                                             \
    typename CTXT::Rule NAME;                                                                      \
    NAME = (EXPR);                                                                                 \
    ::peg::peg_rule_name(NAME, #NAME)

#define PEG_RULE_DEF_LABELED(CTXT, NAME, LABEL, EXPR)                                              \
    typename CTXT::Rule NAME;                                                                      \
    NAME = (EXPR);                                                                                 \
    ::peg::peg_rule_name(NAME, #NAME);                                                             \
    ::peg::peg_rule_label(NAME, LABEL)

#define PEG_RULE_RECURSIVE(CTXT, NAME, EXPR) PEG_RULE_DEF(CTXT, NAME, EXPR)
