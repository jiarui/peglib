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
// ---------------------------------------------------------------------------

#define PEG_RULE(CTXT, NAME, EXPR)                                                                 \
    typename CTXT::Rule NAME = (EXPR);                                                             \
    ::peg::peg_rule_name(NAME, #NAME)

#define PEG_RULE_LABELED(CTXT, NAME, LABEL, EXPR)                                                  \
    typename CTXT::Rule NAME = (EXPR);                                                             \
    ::peg::peg_rule_name(NAME, #NAME);                                                             \
    ::peg::peg_rule_label(NAME, LABEL)
