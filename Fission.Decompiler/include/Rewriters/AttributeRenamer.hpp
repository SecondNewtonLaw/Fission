// Names GetAttribute results and SetAttribute values from their attribute key.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class AttributeRenamer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        ScopeAwareRenamer::Run(
            statements,
            [](const std::vector<std::shared_ptr<Statement>> &scope) {
                // Reading an attribute is the authoritative use of its name, so `:GetAttribute("Damage")`
                // claims `damage` ahead of `:SetAttribute("Damage", v)` (which writes a computed value into
                // it). Collect the two kinds apart, then drop any set-value claim whose leaf a get already
                // owns; otherwise the two would collide on the same name and the engine would refuse both.
                std::vector<std::pair<std::string, std::string>> getCandidates, setCandidates;
                for (const auto &s : scope)
                    CollectCandidate(s, getCandidates, setCandidates);

                std::unordered_set<std::string> getLeaves;
                for (const auto &[var, leaf] : getCandidates)
                    getLeaves.insert(leaf);

                std::vector<std::pair<std::string, std::string>> candidates = std::move(getCandidates);
                for (auto &c : setCandidates)
                    if (!getLeaves.contains(c.second))
                        candidates.push_back(std::move(c));
                return candidates;
            },
            /*includeParams=*/true
        ); // the value written by `:SetAttribute("X", arg)` may be a parameter
    }

  private:
    // Method name of a `obj:Method(...)` namecall.
    static std::string NameCallMethod(const std::shared_ptr<NameCallExpressionNode> &nc) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(nc->callWhat); id && id->identifier)
            return id->identifier->name;
        return "";
    }

    // English word for a single decimal digit, so a name that folds to a leading digit (illegal as an
    // identifier start) can be salvaged: `3DOffset` -> `threeDOffset`. nullptr for a non-digit.
    static const char *DigitWord(char c) {
        switch (c) {
        case '0':
            return "zero";
        case '1':
            return "one";
        case '2':
            return "two";
        case '3':
            return "three";
        case '4':
            return "four";
        case '5':
            return "five";
        case '6':
            return "six";
        case '7':
            return "seven";
        case '8':
            return "eight";
        case '9':
            return "nine";
        default:
            return nullptr;
        }
    }

    // Fold a possibly-multi-word attribute name into a camelCase identifier. Words are whitespace
    // separated: the first keeps its internal casing but is lower-first-cased (`AccuracyDeviation` ->
    // `accuracyDeviation`); each later word becomes Upper-first + lower-rest so acronyms read as words
    // (`Max HP` -> `maxHp`, `Super Long Name` -> `superLongName`). A single word is lower-first. A
    // leading digit is spelled out so the result is a legal identifier start (`3D Offset` -> `threeDOffset`).
    static std::string CamelCase(const std::string &s) {
        std::vector<std::string> words;
        std::string cur;
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) {
                    words.push_back(cur);
                    cur.clear();
                }
            } else
                cur.push_back(c);
        }
        if (!cur.empty())
            words.push_back(cur);
        if (words.empty())
            return "";

        std::string out = ScopeAwareRenamer::LowerFirst(words[0]);
        for (size_t i = 1; i < words.size(); ++i) {
            std::string w = words[i];
            for (char &c : w)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
            out += w;
        }
        // Spell out a leading digit (an identifier cannot start with one); interior digits are legal and
        // left as-is. The following character keeps its case, so `3DOffset` -> `threeDOffset` still reads
        // at the D word boundary.
        if (!out.empty())
            if (const char *word = DigitWord(out[0]))
                out = std::string(word) + out.substr(1);
        return out;
    }

    // A string-literal argument (`"Damage"`, `"Max HP"`) folded to the local name to use (`damage`,
    // `maxHp`), or "" if it is not a string literal or the folded form is not a legal bare identifier
    // (e.g. leading digit, or punctuation the space-fold does not remove).
    static std::string AttrLeaf(const std::shared_ptr<Expression> &arg) {
        auto s = std::dynamic_pointer_cast<StringLiteralNode>(arg);
        if (!s)
            return "";
        const std::string leaf = CamelCase(s->value);
        return ScopeAwareRenamer::IsBareIdentifier(leaf) ? leaf : "";
    }

    // If `expr` is `obj:GetAttribute("Name")`, return the leaf name; else "".
    static std::string GetAttributeLeaf(const std::shared_ptr<Expression> &expr) {
        auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr);
        if (!nc || NameCallMethod(nc) != "GetAttribute" || nc->arguments.empty())
            return "";
        return AttrLeaf(nc->arguments[0]);
    }

    // The identifier name of an expression, if it is a plain identifier read; else "".
    static std::string IdentifierName(const std::shared_ptr<Expression> &e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e); id && id->identifier)
            return id->identifier->name;
        return "";
    }

    static void CollectCandidate(
        const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &getCandidates,
        std::vector<std::pair<std::string, std::string>> &setCandidates
    ) {
        if (!stmt)
            return;
        // `local v = obj:GetAttribute("X")` as an ExpressionStatement (namecall carrying its own return),
        // and `obj:SetAttribute("X", v)` as a bare method-call statement.
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression);
            if (!nc)
                return;
            const std::string method = NameCallMethod(nc);
            if (method == "GetAttribute" && nc->bIsLocalDeclaration && nc->rets.size() == 1) {
                if (const std::string leaf = GetAttributeLeaf(nc); !leaf.empty())
                    if (const std::string ret = IdentifierName(nc->rets[0]); !ret.empty())
                        getCandidates.emplace_back(ret, leaf);
            } else if (method == "SetAttribute" && nc->arguments.size() >= 2) {
                if (const std::string leaf = AttrLeaf(nc->arguments[0]); !leaf.empty())
                    if (const std::string val = IdentifierName(nc->arguments[1]); !val.empty())
                        setCandidates.emplace_back(val, leaf);
            }
            return;
        }
        // `local v = obj:GetAttribute("X")` as a VariableDeclaration.
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (const std::string leaf = GetAttributeLeaf(decl->value); !leaf.empty())
                if (const std::string name = IdentifierName(decl->identifier); !name.empty())
                    getCandidates.emplace_back(name, leaf);
            return;
        }
        // split form: `v = obj:GetAttribute("X")`.
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            if (const std::string leaf = GetAttributeLeaf(asn->right); !leaf.empty())
                if (const std::string name = IdentifierName(asn->left); !name.empty())
                    getCandidates.emplace_back(name, leaf);
            return;
        }
    }
};
