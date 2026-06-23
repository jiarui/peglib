#pragma once
#include "peglib/Concepts.h"
#include "peglib/NonTerminal.h"
#include "peglib/Terminals.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace peg
{

// ---------------------------------------------------------------------------
// Grammar: user-facing container of named rules. Sole owner of all
// NonTerminals (held via shared_ptr).
//
//   Grammar<> g;
//   g["number"] = +terminal('0', '9');
//   g["expr"]   = g["number"] | g["expr"] >> '+' >> g["number"];
//   g.set_start("expr");
//   g.parse_string("1+2+3");  // convenience: creates a Context internally
//
// Rules are lazily created on first access via operator[]. Assignment
// auto-names the rule from the map key. The same Grammar can parse many
// inputs — each parse gets a fresh Context (fresh memo cache, position).
//
// **Design**: Rule (the handle returned by operator[]) stores a bare
// NonTerminal*, not a shared_ptr. This eliminates shared_ptr cycles in
// recursive grammars at the source — ~Grammar() needs no special handling.
// The constraint is that Rule cannot outlive its Grammar (intentional).
// ---------------------------------------------------------------------------
template<typename CharT = char, typename NodeType = std::monostate>
    requires PegContext<Context<CharT, NodeType>>
class Grammar
{
public:
    using Context = peg::Context<CharT, NodeType>;
    using Rule = parsers::Rule<Context>;
    using NonTerminalType = parsers::NonTerminal<Context>;

    Grammar() = default;
    Grammar(const Grammar&) = delete;
    Grammar& operator=(const Grammar&) = delete;
    Grammar(Grammar&&) = default;
    Grammar& operator=(Grammar&&) = default;
    ~Grammar()
    {
#ifndef NDEBUG
        // Debug lifetime aid: poison every NonTerminal's body before the
        // shared_ptr map releases them. If a Rule handle escapes its
        // Grammar's lifetime, its next parse() trips the
        // `assert(m_rule && ...)` in NonTerminal::parseImpl instead of
        // silently using freed memory. (Under ASan, the same misuse is
        // additionally caught as a use-after-free.) No-op in release builds.
        for (auto& [_, nt] : m_rules) {
            nt->clear_body_for_debug();
        }
#endif
    }

    // Access a rule by name. This is **get-or-create**: if the rule does
    // not yet exist it is lazily inserted as a forward declaration (an
    // undefined NonTerminal). Returns a non-owning Rule handle for
    // assignment / chaining / introspection.
    //
    // Caveat: because operator[] inserts on miss, using it for an existence
    // check pollutes undefined_rules() — `if (g["typo"].is_defined())` creates
    // the rule "typo". For read-only existence queries that must NOT insert,
    // use find() or has_rule() instead.
    Rule operator[](const std::string& name)
    {
        auto [it, inserted] = m_rules.try_emplace(name);
        if (inserted) {
            it->second = std::make_shared<NonTerminalType>();
        }
        return Rule{it->second.get(), it->first};
    }

    // Read-only rule lookup. Returns std::nullopt if the rule does not
    // exist — and, unlike operator[], does NOT insert it. Use this (or
    // has_rule) when you need to inspect a rule's existence/body without
    // side-effecting the grammar.
    [[nodiscard]] std::optional<Rule> find(std::string_view name) const
    {
        auto it = m_rules.find(std::string{name});
        if (it == m_rules.end())
            return std::nullopt;
        return Rule{it->second.get(), it->first};
    }

    [[nodiscard]] bool has_rule(std::string_view name) const
    {
        return m_rules.find(std::string{name}) != m_rules.end();
    }

    [[nodiscard]] std::vector<std::string> rule_names() const
    {
        std::vector<std::string> names;
        names.reserve(m_rules.size());
        for (const auto& [name, _] : m_rules) {
            names.push_back(name);
        }
        return names;
    }

    // Start rule management
    void set_start(std::string name) { m_start = std::move(name); }
    [[nodiscard]] const std::string& start_rule() const noexcept { return m_start; }

    // -----------------------------------------------------------------------
    // Auto-skip.
    //
    // Set a transparent "skipper" rule that is invoked automatically
    //   - between adjacent children of a Sequence (static and Dyn),
    //   - between iterations of a repetition (* + n* ?),
    // and nowhere else (not before the first child, not after the last,
    // not inside Alternatives, predicates, or terminal-sequence literals).
    //
    // The rule is typically *e matching whitespace/comments:
    //   g["ws"] = *terminal<char>([](char c){ return c==' '||c=='\t'||c=='\n'||c=='\r'; });
    //   g.set_skipper(g["ws"]);
    // After this, sequences no longer need to thread `>> g["ws"] >>`
    // manually between every pair of terminals.
    //
    // To disable auto-skip for a single sub-expression, wrap it in
    // lexeme(...). To disable globally, call clear_skipper() (or never
    // call set_skipper at all — that is the default).
    //
    // The argument must be a *defined* rule of this Grammar. Grammar
    // stores a non-owning pointer to the rule's NonTerminal (the rule
    // body lives in m_rules for the Grammar's whole lifetime, so the
    // pointer is valid as long as the Grammar is).
    // -----------------------------------------------------------------------
    void set_skipper(Rule r)
    {
        if (!r.is_defined()) {
            throw std::invalid_argument{"Grammar::set_skipper: rule is not defined"};
        }
        m_skipper = r.impl();
    }

    void clear_skipper() noexcept { m_skipper = nullptr; }

    [[nodiscard]] bool has_skipper() const noexcept { return m_skipper != nullptr; }

    // Parse using the start rule. Returns true on success, false on any
    // failure (including cut-committed failures). On failure, the Context
    // holds the diagnostic — retrieve it with `ctx.take_error()`. This method
    // does not throw for parse failures; it only throws `std::logic_error` if
    // no start rule is set.
    bool parse(Context& ctx) const
    {
        if (m_start.empty()) {
            throw std::logic_error{"Grammar::parse: no start rule set"};
        }
        return parse(m_start, ctx);
    }

    // Parse using an explicit rule name. Returns true on success, false on
    // any failure (regular or cut-committed). Cut-committed failures (which
    // manifest internally as a thrown peg::ParseError from the
    // Alternation/Repetition that owned the cut scope) are caught here and
    // surfaced as a normal failure: the Context's furthest-failure state —
    // already populated by record_failure() calls made before the throw — is
    // queryable via ctx.take_error(). Throws std::out_of_range if `rule`
    // is not defined.
    bool parse(std::string_view rule, Context& ctx) const
    {
        auto it = m_rules.find(std::string{rule});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::parse: rule '" + std::string{rule} + "' not found"};
        }
        // Stamp the Grammar-owned skipper onto the Context for the duration
        // of this parse. No-op (sets nullptr) when the user never called
        // set_skipper, in which case run_skipper() is a zero-cost no-op.
        ctx.internal_set_skipper(m_skipper);
        // Pest-style leading whitespace: consume at the grammar boundary
        // so users don't have to prefix `g["ws"] >>` to their start rule.
        // Trailing whitespace is intentionally NOT consumed here —
        // parse_string does partial-match, and users who need full-input
        // consumption append `>> !.` (EndOfFile) to their start rule.
        ctx.run_skipper();
        try {
            return it->second->parse(ctx).success;
        } catch (const ParseError&) {
            // The Context's error state was already updated by record_failure
            // calls along the failing path before the cut committed. Nothing
            // to do here but report failure.
            return false;
        }
    }

    // Parse and return the parse tree (nullptr on failure or if the start
    // rule is transparent). Useful for AST construction. Like parse(), this
    // catches cut-committed failures and returns nullptr; retrieve the
    // diagnostic via ctx.take_error(). Throws std::out_of_range if `rule`
    // is not defined.
    typename Context::ParseTreeNodePtr parse_tree(std::string_view rule, Context& ctx) const
    {
        auto it = m_rules.find(std::string{rule});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::parse_tree: rule '" + std::string{rule} +
                                    "' not found"};
        }
        ctx.internal_set_skipper(m_skipper);
        ctx.run_skipper(); // leading whitespace (pest-style; see parse() above)
        try {
            return it->second->parse(ctx).tree;
        } catch (const ParseError&) {
            return nullptr;
        }
    }

    // Convenience: parse a string input using the start rule.
    //
    // Partial-match semantics: returns true if the start rule matches at the
    // beginning of `input`, EVEN IF input remains unconsumed. To require the
    // whole input be consumed, append a end-of-input anchor (!. / EndOfFile)
    // to the start rule in the grammar. (parse_string makes its own copy of
    // the input, so temporaries are safe to pass.)
    bool parse_string(std::string_view input) const
    {
        std::string s{input};
        Context ctx{s};
        return parse(ctx);
    }

    // -----------------------------------------------------------------------
    // Validation helpers
    // -----------------------------------------------------------------------

    // Returns names of rules that were accessed via operator[] but never
    // assigned a definition (still empty NonTerminal). These would silently
    // fail to match at parse time.
    [[nodiscard]] std::vector<std::string> undefined_rules() const
    {
        std::vector<std::string> result;
        for (const auto& [name, nt] : m_rules) {
            if (!nt->is_defined()) {
                result.push_back(name);
            }
        }
        return result;
    }

    // Returns names of rules that are defined but not reachable from the
    // start rule. These are dead code — they can be removed without
    // changing the grammar's behaviour.
    [[nodiscard]] std::vector<std::string> unreachable_rules() const
    {
        if (m_start.empty())
            return {};
        auto start_it = m_rules.find(m_start);
        if (start_it == m_rules.end())
            return {};

        // Collect all rule names transitively referenced from the start
        // rule's body.
        std::set<std::string> reachable;
        reachable.insert(m_start);

        // DFS: for each reachable rule, collect its body's direct references.
        std::vector<std::string> queue{m_start};
        while (!queue.empty()) {
            auto name = queue.back();
            queue.pop_back();
            auto it = m_rules.find(name);
            if (it == m_rules.end())
                continue;

            std::set<std::string> refs;
            it->second->collect_rule_refs(refs);
            for (const auto& ref : refs) {
                if (reachable.insert(ref).second) {
                    queue.push_back(ref);
                }
            }
        }

        // Unreachable = all defined rules minus reachable.
        std::vector<std::string> result;
        for (const auto& [name, nt] : m_rules) {
            if (nt->is_defined() && reachable.find(name) == reachable.end()) {
                result.push_back(name);
            }
        }
        return result;
    }

    // -----------------------------------------------------------------------
    // to_dot: Graphviz DOT digraph of rule dependencies.
    //
    // Emits every defined rule as a node (the start rule gets a double
    // border via peripheries=2), and every rule-reference (collected via
    // collect_rule_refs) as a directed edge. Undefined references appear
    // as dangling edge targets — useful for spotting typos. The output
    // is suitable for piping through `dot -Tsvg` / `dot -Tpng`:
    //
    //   std::cout << g.to_dot();
    //   // then:  ./my_parser | dot -Tsvg > grammar.svg
    //
    // Implementation reuses the same collect_rule_refs traversal that
    // unreachable_rules() does (every expression type overrides it), so
    // no new virtual is needed. Traversal is DFS over (rule -> direct
    // refs); the visited set bounds work at O(rules + edges).
    // -----------------------------------------------------------------------
    [[nodiscard]] std::string to_dot() const
    {
        std::string out;
        out.reserve(256);
        out += "digraph peglib_grammar {\n";
        out += "  node [shape=box];\n";
        out += "  rankdir=LR;\n";

        // Defined-rule nodes (start rule highlighted with a double border).
        for (const auto& [name, nt] : m_rules) {
            if (!nt->is_defined())
                continue;
            out += "  \"";
            out += dot_escape(name);
            out += "\"";
            if (name == m_start) {
                out += " [peripheries=2]";
            }
            out += ";\n";
        }

        // Edges via collect_rule_refs. Visit every defined rule (not just
        // those reachable from start) so dead-code branches still render
        // for inspection. The visited set prevents re-emitting a rule's
        // edges when it appears multiple times in the queue.
        std::set<std::string> visited;
        std::vector<std::string> queue;
        for (const auto& [name, nt] : m_rules) {
            if (nt->is_defined()) {
                queue.push_back(name);
            }
        }
        while (!queue.empty()) {
            auto name = queue.back();
            queue.pop_back();
            if (!visited.insert(name).second)
                continue;
            auto it = m_rules.find(name);
            if (it == m_rules.end())
                continue;

            std::set<std::string> refs;
            it->second->collect_rule_refs(refs);
            for (const auto& ref : refs) {
                out += "  \"";
                out += dot_escape(name);
                out += "\" -> \"";
                out += dot_escape(ref);
                out += "\";\n";
                // Chase the reference (defined refs add their own edges).
                if (m_rules.count(ref)) {
                    queue.push_back(ref);
                }
            }
        }
        out += "}\n";
        return out;
    }

protected:
    // Grammar is the sole owner of all NonTerminals. Rule handles (returned
    // by operator[]) hold bare NonTerminal* — no shared_ptr cycle possible.
    std::map<std::string, std::shared_ptr<NonTerminalType>> m_rules;
    std::string m_start;

    // Auto-skip rule. Non-owning pointer into m_rules; set by
    // set_skipper, cleared by clear_skipper. nullptr = no auto-skip.
    // Stamped onto each Context at parse()/parse_tree() entry so that
    // Sequence / Repetition expressions can call ctx.run_skipper() with
    // zero per-parse setup.
    NonTerminalType* m_skipper = nullptr;

    // Escape a rule name for safe embedding inside a DOT string literal.
    // DOT recognises \" \\ and \n inside "..."; everything else passes
    // through. Used only by to_dot().
    static std::string dot_escape(std::string_view s)
    {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out += c;
            }
        }
        return out;
    }
};

} // namespace peg
