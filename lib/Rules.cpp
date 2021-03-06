#include <Rules.h>

#include <Common.h>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <set>
#include <span>
#include <string>
#include <string_view>

using namespace ffps;

static std::string GetLine(std::ifstream &ifile) {
  std::string line;
  std::getline(ifile, line);
  return line;
}

static std::vector<std::string_view> SplitLine(std::string_view line) {
  std::vector<std::string_view> splitedLine;
  constexpr char delim = ' ';
  size_t prev = 0, pos = 0;
  do {
    pos = line.find(delim, prev);
    if (pos == std::string::npos) {
      pos = line.length();
    }

    std::string_view token = line.substr(prev, pos - prev);

    if (!token.empty()) {
      splitedLine.push_back(token);
    }

    prev = pos + 1;
  } while (pos < line.length() && prev < line.length());

  return splitedLine;
}

static std::size_t GetDiscondancePos(const std::vector<std::string> &prod1,
                                     const std::vector<std::string> &prod2) {
  assert(!prod1.empty() && !prod2.empty() &&
         "The productions can not be empty!");

  std::size_t i = 0;
  for (; i < prod1.size() && i < prod2.size(); ++i) {
    if (prod1[i] != prod2[i]) {
      return i;
    }
  }
  return i;
}

std::vector<std::string>
GetCommonFactors(const std::vector<std::string> &prod1,
                 const std::vector<std::string> &prod2) {
  if (prod1.empty() || prod2.empty()) {
    return prod1.empty() ? prod2 : prod1;
  }

  std::size_t discordancePos = GetDiscondancePos(prod1, prod2);
  return {prod1.begin(), std::next(prod1.begin(), discordancePos)};
}

static std::set<std::string_view>
GetLeftSymbolsRepeatedMoreThanOne(const Productions &productions) {
  std::unordered_map<std::string_view, std::size_t> map;

  std::ranges::for_each(productions,
                        [&](const Production &prod) { map[prod.front()]++; });

  std::set<std::string_view> result;
  for (auto &[symbol, ocurrences] : map) {
    if (ocurrences > 1) {
      result.insert(symbol);
    }
  }

  return result;
}

static std::unordered_map<std::string_view, std::vector<std::string>>
GetLeftCommonSymbols(const Productions &productions) {

  std::set<std::string_view> repeatedSymbols =
      GetLeftSymbolsRepeatedMoreThanOne(productions);

  std::unordered_map<std::string_view, std::vector<std::string>> result;

  for (const Production &prod2 : productions) {
    if (!repeatedSymbols.contains(prod2.front())) {
      continue;
    }

    Production &prod1 = result[prod2.front()];
    prod1 = GetCommonFactors(prod1, prod2);
  }

  return result;
}

static void SolveCommonFactorsByTheLeft(Rules &rules) {
  bool changed = true;

  while (changed) {
    changed = false;
    for (auto &[symbol, productions] : rules) {
      auto leftCommonSymbolsMap = GetLeftCommonSymbols(productions);
      if (leftCommonSymbolsMap.empty()) {
        continue;
      }

      std::string newRule = symbol + "_CFL";
      Productions newProductions;
      bool alreadyAddedEpsilon = false;

      for (Production &prod : productions) {
        if (!leftCommonSymbolsMap.contains(prod.front())) {
          continue;
        }

        // Add the common part to a new production of the new rule
        std::size_t discordancePos =
            GetDiscondancePos(leftCommonSymbolsMap[prod.front()], prod);

        // Add epsilon in case of unique symbol and not added yet
        if (!alreadyAddedEpsilon && prod.size() == discordancePos) {
          alreadyAddedEpsilon = true;
          newProductions.push_back({EpsilonStr.data()});
          prod.clear();
          continue;
        }

        newProductions.push_back(
            {std::next(prod.begin(), discordancePos), prod.end()});

        // Remove from production being factored
        prod.erase(std::next(prod.begin(), discordancePos), prod.end());
        // Append new rule
        prod.push_back(newRule);
      }

      // Append new productions
      rules[newRule] = std::move(newProductions);

      // Remove duplicates generated by left factoring
      std::sort(productions.begin(), productions.end());
      productions.erase(unique(productions.begin(), productions.end()),
                        productions.end());

      // Remove empty productions
      const auto it =
          std::remove_if(productions.begin(), productions.end(),
                         [](const Production &prod) { return prod.empty(); });
      productions.erase(it, productions.end());

      changed = true;
      break;
    }
  }
}

static void SolveLeftRecursion(Rules &rules) {
  for (auto &[symbol, ruleProductions] : rules) {

    // Recover all productions that start the the rule
    std::vector<Productions::iterator> productionsWithRulePrefix;

    for (auto it = ruleProductions.begin(), itEnd = ruleProductions.end();
         it != itEnd; it++) {
      Production &production = *it;
      assert(production.size() && "Production without elements!");
      // If rule is same as its first symbol of its production
      if (symbol == production.front()) {
        productionsWithRulePrefix.push_back(it);
      }
    }

    if (productionsWithRulePrefix.empty()) {
      continue;
    }

    // Create new Rule with _LR suffix and with new symbols
    std::string newRule = symbol + "_LR";
    Productions newProductions;

    // Create new productions, one for each production starting with the non
    // terminal symbol of the rule
    for (auto it : productionsWithRulePrefix) {
      Production &production = *it;
      std::vector<std::string> newProduction(std::next(production.begin(), 1),
                                             production.end());
      newProduction.push_back(newRule);

      newProductions.push_back(std::move(newProduction));
    }
    // Append epsilon
    newProductions.push_back({EpsilonStr.data()});

    // Remove from the original production
    const auto it =
        std::remove_if(ruleProductions.begin(), ruleProductions.end(),
                       [&symbol = symbol](const auto &production) {
                         return symbol == production.front();
                       });
    ruleProductions.erase(it, ruleProductions.end());

    // Append to the end of all the productions
    for (std::vector<std::string> &production : ruleProductions) {
      production.push_back(newRule);
    }

    // Insert new production
    rules[std::move(newRule)] = std::move(newProductions);
  }
}

static bool IsSeparator(std::string_view data) { return data == "->"; }

using PartialRule = struct {
  std::string Rule;
  Production Production;
};

static std::optional<PartialRule> GetPartialRule(std::string_view line) {
  std::vector<std::string_view> splitedLine = SplitLine(line);
  // At least "Rule -> Symbol"
  if (splitedLine.size() < 3) {
    return std::nullopt;
  }

  if (!IsSeparator(splitedLine[1])) {
    return std::nullopt;
  }

  std::string_view ruleOfProduction = splitedLine[0];
  auto production = std::span<std::string_view>{
      std::next(splitedLine.begin(), 2), splitedLine.end()};

  return PartialRule{std::string(ruleOfProduction),
                     {production.begin(), production.end()}};
}

static std::optional<Rules> ExtractRules(std::ifstream &ifile) {
  Rules rules;
  while (!ifile.eof()) {
    std::string line = GetLine(ifile);
    if (line.empty()) {
      continue;
    }

    std::optional<PartialRule> rule = GetPartialRule(line);
    if (!rule) {
      std::cerr << "Error trying to get rule for line:\n\t " << line << '\n';
      return std::nullopt;
    }

    rules[rule->Rule].push_back(std::move(rule->Production));
  }
  return rules;
}

struct IndirectLeftRecursionInfo {
  std::string_view Symbol;
  std::set<std::string> FirstSymbolOfEachProduction;
};

static bool HasIndirectLeftRecursion(const Rules &rules) {
  std::unordered_map<std::string_view, std::set<std::string>> ILLIs;
  for (auto &[symbol, productions] : rules) {
    IndirectLeftRecursionInfo ILLI;
    ILLI.Symbol = symbol;
    for (auto production : productions) {
      auto frontSym = production.front();
      if (IsNonTerminal(rules, frontSym) && symbol != frontSym) {
        ILLI.FirstSymbolOfEachProduction.insert(frontSym);
      }
    }
    ILLIs.insert({ILLI.Symbol, std::move(ILLI.FirstSymbolOfEachProduction)});
  }

  for (auto &[symbol, firstSymbolOfEachProduction] : ILLIs) {
    std::set<std::string_view> symbolsAlreadySeen;
    std::set<std::string_view> symbolsToSee({symbol});

    while (!symbolsToSee.empty()) {
      std::string_view symbolToSee = *symbolsToSee.begin();
      symbolsToSee.erase(symbolToSee);

      assert(ILLIs.contains(symbolToSee) && "symbolToSee not found in map!");
      if (ILLIs[symbolToSee].contains(symbol.data())) {
        return true;
      }

      if (const auto [sym, ok] = symbolsAlreadySeen.insert(symbolToSee); ok) {
        symbolsToSee.insert(ILLIs[symbolToSee].begin(),
                            ILLIs[symbolToSee].end());
      }
    }
  }

  return false;
}

std::optional<Rules> ffps::BuildRules(std::string_view file,
                                      bool solveLeftRecursion,
                                      bool solveCommonFactorsByTheLeft) {
  std::ifstream ifile(file.data());

  if (!ifile) {
    std::cerr << "File \"" << file << "\" not found!\n";
    return std::nullopt;
  }

  std::optional<Rules> rules = ExtractRules(ifile);
  if (!rules) {
    return std::nullopt;
  }

  if (HasIndirectLeftRecursion(*rules)) {
    std::cerr << "Indirect left recursion not supported yet!\n";
    return std::nullopt;
  }

  if (solveLeftRecursion) {
    SolveLeftRecursion(*rules);
  }

  if (solveCommonFactorsByTheLeft) {
    SolveCommonFactorsByTheLeft(*rules);
  }

  return rules;
}

void ffps::Print(const Rules &rules, std::ostream &out) {
  for (auto &[rule, productions] : rules) {
    out << rule << '\n';
    for (Production production : productions) {
      out << '\t';
      for (std::string_view symbol : production) {
        out << symbol << ' ';
      }
      out << '\n';
    }
  }
}
