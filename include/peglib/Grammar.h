#pragma once
#include "peglib/NonTerminal.h"
#include "peglib/Terminals.h"

#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace peg
{

// ---------------------------------------------------------------------------
// Grammar: user-facing container of named rules.
//
//   Grammar<> g;
//   g["number"] = +terminal('0', '9');
//   g["expr"]   = g["number"] | g["expr"] >> '+' >> g["number"];
//   g.set_start("expr");
//   g.parse_string("1+2+3");  // convenience: creates a Context internally
//
// Rules are lazily created on first access via operator[]. Assignment
// auto-names the rule from the map key. The same Grammar can parse many
// inputs — each parse gets a fresh Context (fresh memo cache, position,
// value stack).
// ---------------------------------------------------------------------------
template<typename Ctx = Context<std::span<const char>>>
class Grammar
{
public:
    using Context = Ctx;
    using Rule = parsers::Rule<Context>;
    using RuleProxy = parsers::RuleProxy<Context>;

    Grammar() = default;
    Grammar(const Grammar&) = delete;
    Grammar& operator=(const Grammar&) = delete;
    Grammar(Grammar&&) = default;
    Grammar& operator=(Grammar&&) = default;

    // Access a rule by name. Lazily creates the rule if it doesn't exist
    // (forward declaration). Returns a RuleProxy for assignment/chaining.
    RuleProxy operator[](std::string name)
    {
        auto [it, inserted] = m_rules.try_emplace(name);
        return RuleProxy{it->second, std::move(name)};
    }

    // Const access — throws if the rule doesn't exist.
    const Rule& at(std::string_view name) const
    {
        auto it = m_rules.find(std::string{name});
        if (it == m_rules.end()) {
            throw std::out_of_range{"Grammar::at: rule '" + std::string{name} + "' not found"};
        }
        return it->second;
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

    // Parse using the start rule. Returns true on success.
    bool parse(Context& ctx) const
    {
        if (m_start.empty()) {
            throw std::logic_error{"Grammar::parse: no start rule set"};
        }
        return parse(m_start, ctx);
    }

    // Parse using an explicit rule name. Returns true on success.
    bool parse(std::string_view rule, Context& ctx) const { return at(rule).parse(ctx).success; }

    // Parse and return the parse tree (nullptr on failure or if the start
    // rule is transparent). Useful for AST construction.
    typename Context::ParseTreeNodePtr parse_tree(std::string_view rule, Context& ctx) const
    {
        return at(rule).parse(ctx).tree;
    }

    // Convenience: parse a string input using the start rule.
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
        for (const auto& [name, rule] : m_rules) {
            if (!rule.is_defined()) {
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
            it->second.collect_rule_refs(refs);
            for (const auto& ref : refs) {
                if (reachable.insert(ref).second) {
                    queue.push_back(ref);
                }
            }
        }

        // Unreachable = all defined rules minus reachable.
        std::vector<std::string> result;
        for (const auto& [name, rule] : m_rules) {
            if (rule.is_defined() && reachable.find(name) == reachable.end()) {
                result.push_back(name);
            }
        }
        return result;
    }

protected:
    std::map<std::string, Rule> m_rules;
    std::string m_start;
};

} // namespace peg
