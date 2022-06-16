//===--- MinimalConformances.cpp - Reasoning about conformance rules ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements an algorithm to find a minimal set of conformance
// rules (V1.[P1] => V1), ..., (Vn.[Pn] => Vn) whose left hand sides
// _generate_ the set of conformance-valid terms.
//
// That is, any valid term of the form T.[P] where T.[P] reduces to T can be
// written as a product of terms (Vi.[Pi]), where each Vi.[Pi] is a left hand
// side of a minimal conformance.
//
// A "conformance-valid" rewrite system is one where if we can write
// T == U.V for arbitrary non-empty U and V, then U.[domain(V)] is joinable
// with U.
//
// If this holds, then starting with a term T.[P] that is joinable with T, we
// can reduce T to canonical form T', and find the unique rule (V.[P] => V) such
// that T' == U.V. Then we repeat this process with U.[domain(V)], which is
// known to be joinable with U, since T is conformance-valid.
//
// Iterating this process produces a decomposition of T.[P] as a product of
// left hand sides of conformance rules. Some of those rules are not minimal;
// they are added by completion, or they are redundant rules written by the
// user.
//
// Using the rewrite loops that generate the homotopy relation on rewrite paths,
// decompositions can be found for all "derived" conformance rules, producing
// a set of minimal conformances.
//
// There are two small complications to handle implementation details of
// Swift generics:
//
// 1) Inherited witness tables must be derivable by following other protocol
//    refinement requirements only, without looking at non-Self associated
//    types. This is expressed by saying that the minimal conformance
//    equations for a protocol refinement can only be written in terms of
//    other protocol refinements; conformance paths involving non-Self
//    associated types are not considered.
//
// 2) The subject type of each conformance requirement must be derivable at
//    runtime as well, so for each minimal conformance, it must be
//    possible to write down a conformance path for the parent type without
//    using any minimal conformance recursively in the parent path of
//    itself.
//
// The minimal conformances algorithm finds fewer conformance requirements to be
// redundant than homotopy reduction, which is why homotopy reduction only
// deletes non-protocol conformance requirements.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Range.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include "RewriteContext.h"
#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

/// Finds all protocol conformance rules appearing in a rewrite loop, both
/// in empty context, and with a non-empty left context. Applications of rules
/// with a non-empty right context are ignored.
///
/// The rules are organized by protocol. For each protocol, the first element
/// of the pair stores conformance rules that appear without context. The
/// second element of the pair stores rules that appear with non-empty left
/// context. For each such rule, the left prefix is also stored alongside.
void RewriteLoop::findProtocolConformanceRules(
    llvm::SmallDenseMap<const ProtocolDecl *,
                        ProtocolConformanceRules, 2> &result,
    const RewriteSystem &system) const {

  auto redundantRules = findRulesAppearingOnceInEmptyContext(system);

  bool foundAny = false;
  for (unsigned ruleID : redundantRules) {
    const auto &rule = system.getRule(ruleID);

    if (auto *proto = rule.isAnyConformanceRule()) {
      if (rule.isIdentityConformanceRule()) {
        result[proto].SawIdentityConformance = true;
        continue;
      }

      result[proto].RulesInEmptyContext.push_back(ruleID);
      foundAny = true;
    }
  }

  if (!foundAny)
    return;

  RewritePathEvaluator evaluator(Basepoint);

  // Now look for rewrite steps with conformance rules in empty right context,
  // that is something like X.(Y.[P] => Y) (or it's inverse, X.(Y => Y.[P])).
  for (const auto &step : Path) {
    if (!evaluator.isInContext()) {
      switch (step.Kind) {
      case RewriteStep::Rule: {
        const auto &rule = system.getRule(step.getRuleID());

        if (rule.isIdentityConformanceRule())
          break;

        if (auto *proto = rule.isAnyConformanceRule()) {
          if (step.StartOffset > 0 &&
              step.EndOffset == 0) {
            // Record the prefix term that is left unchanged by this rewrite step.
            //
            // In the above example where the rewrite step is X.(Y.[P] => Z),
            // the prefix term is 'X'.
            const auto &term = evaluator.getCurrentTerm();
            MutableTerm prefix(term.begin(), term.begin() + step.StartOffset);
            result[proto].RulesInContext.emplace_back(prefix, step.getRuleID());
          }
        }

        break;
      }

      case RewriteStep::PrefixSubstitutions:
      case RewriteStep::Shift:
      case RewriteStep::Decompose:
      case RewriteStep::Relation:
      case RewriteStep::DecomposeConcrete:
      case RewriteStep::LeftConcreteProjection:
      case RewriteStep::RightConcreteProjection:
        break;
      }
    }

    evaluator.apply(step, system);
  }
}

namespace {

/// Utility class to encapsulate some shared state.
class MinimalConformances {
  const RewriteSystem &System;

  RewriteContext &Context;

  DebugOptions Debug;

  // All conformance rules in the current minimization domain, sorted by
  // (isExplicit(), getLHS()), with non-explicit rules with longer left hand
  // sides coming first.
  //
  // The idea here is that we want less canonical rules to be eliminated first,
  // but we prefer to eliminate non-explicit rules, in an attempt to keep protocol
  // conformance rules in the same protocol as they were originally defined in.
  SmallVector<unsigned, 4> ConformanceRules;

  // Maps a conformance rule in the current minimization domain to a conformance
  // path deriving the subject type's base type. For example, consider the
  // following conformance rule:
  //
  //   T.[P:A].[Q:B].[R] => T.[P:A].[Q:B]
  //
  // The subject type is T.[P:A].[Q:B]; in order to derive the metadata, we need
  // the witness table for T.[P:A] : [Q] first, by computing a conformance access
  // path for the term T.[P:A].[Q], known as the 'parent path'.
  llvm::MapVector<unsigned, SmallVector<unsigned, 2>> ParentPaths;

  // Maps a conformance rule in the current minimization domain to a list of paths.
  // Each path in the list is a unique derivation of the conformance in terms of
  // other conformance rules.
  llvm::MapVector<unsigned, std::vector<SmallVector<unsigned, 2>>> ConformancePaths;

  // The set of conformance rules (from all minimization domains) which are protocol
  // refinements, that is rules of the form [P].[Q] => [P].
  llvm::DenseSet<unsigned> ProtocolRefinements;

  // This is the computed result set of redundant conformance rules in the current
  // minimization domain.
  llvm::DenseSet<unsigned> &RedundantConformances;

  void decomposeTermIntoConformanceRuleLeftHandSides(
      MutableTerm term,
      SmallVectorImpl<unsigned> &result) const;
  void decomposeTermIntoConformanceRuleLeftHandSides(
      MutableTerm term, unsigned ruleID,
      SmallVectorImpl<unsigned> &result) const;

  bool isConformanceRuleRecoverable(
    llvm::SmallDenseSet<unsigned, 4> &visited,
    unsigned ruleID) const;

  bool isDerivedViaCircularConformanceRule(
      const std::vector<SmallVector<unsigned, 2>> &paths) const;

  bool isValidConformancePath(
      llvm::SmallDenseSet<unsigned, 4> &visited,
      const llvm::SmallVectorImpl<unsigned> &path) const;

  bool isValidRefinementPath(
      const llvm::SmallVectorImpl<unsigned> &path) const;

  void dumpConformancePath(
      llvm::raw_ostream &out,
      const SmallVectorImpl<unsigned> &path) const;

  void dumpMinimalConformanceEquation(
      llvm::raw_ostream &out,
      unsigned baseRuleID,
      const std::vector<SmallVector<unsigned, 2>> &paths) const;

public:
  explicit MinimalConformances(const RewriteSystem &system,
                               llvm::DenseSet<unsigned> &redundantConformances)
    : System(system),
      Context(system.getRewriteContext()),
      Debug(system.getDebugOptions()),
      RedundantConformances(redundantConformances) {}

  void collectConformanceRules();

  void computeCandidateConformancePaths();

  void dumpMinimalConformanceEquations(llvm::raw_ostream &out) const;

  void verifyMinimalConformanceEquations() const;

  void computeMinimalConformances();

  void verifyMinimalConformances() const;

  void dumpMinimalConformances(llvm::raw_ostream &out) const;
};

} // end namespace

/// Write the term as a product of left hand sides of protocol conformance
/// rules.
///
/// The term should be irreducible, except for a protocol symbol at the end.
void
MinimalConformances::decomposeTermIntoConformanceRuleLeftHandSides(
    MutableTerm term, SmallVectorImpl<unsigned> &result) const {
  assert(term.back().getKind() == Symbol::Kind::Protocol);

  // If T is canonical and T.[P] => T, then by confluence, T.[P]
  // reduces to T in a single step, via a rule V.[P] => V, where
  // T == U.V.
  RewritePath steps;
  bool simplified = System.simplify(term, &steps);
  if (!simplified) {
    llvm::errs() << "Term does not conform to protocol: " << term << "\n";
    System.dump(llvm::errs());
    abort();
  }

  assert(steps.size() == 1 &&
         "Canonical conformance term should simplify in one step");

  const auto &step = *steps.begin();

#ifndef NDEBUG
  const auto &rule = System.getRule(step.getRuleID());
  assert(rule.isAnyConformanceRule());
  assert(!rule.isIdentityConformanceRule());
#endif

  assert(step.Kind == RewriteStep::Rule);
  assert(step.EndOffset == 0);
  assert(!step.Inverse);

  // If |U| > 0, recurse with the term U.[domain(V)]. Since T is
  // canonical, we know that U is canonical as well.
  if (step.StartOffset > 0) {
    // Build the term U.
    MutableTerm prefix(term.begin(), term.begin() + step.StartOffset);

    decomposeTermIntoConformanceRuleLeftHandSides(
        prefix, step.getRuleID(), result);
  } else {
    result.push_back(step.getRuleID());
  }
}

/// Given a term U and a rule (V.[P] => V), write U.[domain(V)] as a
/// product of left hand sdies of conformance rules. The term U should
/// be irreducible.
void
MinimalConformances::decomposeTermIntoConformanceRuleLeftHandSides(
    MutableTerm term, unsigned ruleID,
    SmallVectorImpl<unsigned> &result) const {
  const auto &rule = System.getRule(ruleID);
  assert(rule.isAnyConformanceRule());
  assert(!rule.isIdentityConformanceRule());

  // Compute domain(V).
  const auto &lhs = rule.getLHS();
  auto protocol = Symbol::forProtocol(lhs[0].getProtocol(), Context);

  // A same-type requirement of the form 'Self.Foo == Self' can induce a
  // conformance rule [P].[P] => [P], and we can end up with a minimal
  // conformance decomposition of the form
  //
  //   (V.[Q] => V) := [P].(V'.[Q] => V'),
  //
  // where domain(V) == [P]. Don't recurse on [P].[P] here since it won't
  // yield anything useful, instead just return with (V'.[Q] => V').
  if (term.size() == 1 && term[0] == protocol) {
    result.push_back(ruleID);
    return;
  }

  // Build the term U.[domain(V)].
  term.add(protocol);

  decomposeTermIntoConformanceRuleLeftHandSides(term, result);

  // Add the rule V => V.[P].
  result.push_back(ruleID);
}

static const ProtocolDecl *getParentConformanceForTerm(Term lhs) {
  // The last element is a protocol symbol, because this is the left hand side
  // of a conformance rule.
  assert(lhs.back().getKind() == Symbol::Kind::Protocol ||
         lhs.back().getKind() == Symbol::Kind::ConcreteConformance);

  // The second to last symbol is either an associated type, protocol or generic
  // parameter symbol.
  assert(lhs.size() >= 2);

  auto parentSymbol = lhs[lhs.size() - 2];

  switch (parentSymbol.getKind()) {
  case Symbol::Kind::AssociatedType: {
    // In a conformance rule of the form [P:T].[Q] => [P:T], the parent type is
    // trivial.
    if (lhs.size() == 2)
      return nullptr;

    // If we have a rule of the form X.[P:Y].[Q] => X.[P:Y] wih non-empty X,
    // then the parent type is X.[P].
    return parentSymbol.getProtocol();
  }

  case Symbol::Kind::GenericParam:
  case Symbol::Kind::Protocol:
    // The parent type is trivial (either a generic parameter, or the protocol
    // 'Self' type).
    return nullptr;

  case Symbol::Kind::Name:
  case Symbol::Kind::Layout:
  case Symbol::Kind::Superclass:
  case Symbol::Kind::ConcreteType:
  case Symbol::Kind::ConcreteConformance:
    break;
  }

  llvm::errs() << "Bad symbol in " << lhs << "\n";
  abort();
}

/// Collect conformance rules and parent paths, and record an initial
/// equation where each conformance is equivalent to itself.
void MinimalConformances::collectConformanceRules() {
  // Prepare the initial set of equations.
  for (unsigned ruleID : indices(System.getRules())) {
    const auto &rule = System.getRule(ruleID);
    if (rule.isPermanent())
      continue;

    if (rule.isRedundant())
      continue;

    if (rule.isRHSSimplified() ||
        rule.isSubstitutionSimplified())
      continue;

    if (rule.containsUnresolvedSymbols())
      continue;

    if (!rule.isAnyConformanceRule())
      continue;

    // Save protocol refinement relations in a side table.
    if (rule.isProtocolRefinementRule(Context))
      ProtocolRefinements.insert(ruleID);

    if (!System.isInMinimizationDomain(rule.getLHS().getRootProtocol()))
      continue;

    ConformanceRules.push_back(ruleID);

    auto lhs = rule.getLHS();

    // Record a parent path if the subject type itself requires a non-trivial
    // conformance path to derive.
    if (auto *parentProto = getParentConformanceForTerm(lhs)) {
      MutableTerm mutTerm(lhs.begin(), lhs.end() - 2);
      assert(!mutTerm.empty());

      mutTerm.add(Symbol::forProtocol(parentProto, Context));

      // Get a conformance path for X.[P] and record it.
      decomposeTermIntoConformanceRuleLeftHandSides(mutTerm, ParentPaths[ruleID]);
    }
  }

  // Sort the list of conformance rules in reverse order; we're going to try
  // to minimize away less canonical rules first.
  std::stable_sort(ConformanceRules.begin(), ConformanceRules.end(),
                   [&](unsigned lhs, unsigned rhs) -> bool {
                     const auto &lhsRule = System.getRule(lhs);
                     const auto &rhsRule = System.getRule(rhs);

                     if (lhsRule.isExplicit() != rhsRule.isExplicit())
                       return !lhsRule.isExplicit();

                     auto result = lhsRule.getLHS().compare(rhsRule.getLHS(), Context);

                     // Concrete conformance rules are unordered if they name the
                     // same protocol but have different types. This can come up
                     // if we have a class inheritance relationship 'Derived : Base',
                     // and Base conforms to a protocol:
                     //
                     //     T.[Base : P] => T
                     //     T.[Derived : P] => T
                     return (result ? *result > 0 : 0);
                   });

  Context.ConformanceRulesHistogram.add(ConformanceRules.size());
}

/// Use homotopy information to discover all ways of writing the left hand side
/// of each conformance rule as a product of left hand sides of other conformance
/// rules.
///
/// Each conformance rule (Vi.[P] => Vi) can always be written in terms of itself,
/// so the first term of each disjunction is always (Vi.[P] => Vi).
///
/// Conformance rules can also be circular, so not every choice of disjunctions
/// produces a valid result; for example, if you have these definitions:
///
///   protocol P {
///     associatedtype T : P
///   }
///
///   struct G<X, Y> where X : P, X.T == Y, Y : P, Y.T == X {}
///
/// We have three conformance rules:
///
///   [P:T].[P] => [P:T]
///   <X>.[P] => <X>
///   <Y>.[P] => <Y>
///
/// The first rule, <X>.[P] => <X> has an alternate conformance path:
///
///   (<Y>.[P]).([P:T].[P])
///
/// The second rule similarly has an alternate conformance path:
///
///   (<X>.[P]).([P:T].[P])
///
/// This gives us the following initial set of candidate conformance paths:
///
///   [P:T].[P] := ([P:T].[P])
///   <X>.[P] := (<X>.[P]) ∨ (<Y>.[P]).([P:T].[P])
///   <Y>.[P] := (<Y>.[P]) ∨ (<X>.[P]).([P:T].[P])
///
/// One valid solution is the following set of assignments:
///
///   [P:T].[P] := ([P:T].[P])
///   <X>.[P] := (<X>.[P])
///   <Y>.[P] := (<X>.[P]).([P:T].[P])
///
/// That is, we can choose to eliminate <X>.[P], but not <Y>.[P], or vice
/// versa; but it is never valid to eliminate both.
void MinimalConformances::computeCandidateConformancePaths() {
  for (unsigned loopID : indices(System.getLoops())) {
    const auto &loop = System.getLoops()[loopID];

    if (loop.isDeleted())
      continue;

    llvm::SmallDenseMap<const ProtocolDecl *,
                        ProtocolConformanceRules, 2> result;

    loop.findProtocolConformanceRules(result, System);

    if (result.empty())
      continue;

    if (Debug.contains(DebugFlags::MinimalConformances)) {
      llvm::dbgs() << "Candidate homotopy generator: (#" << loopID << ") ";
      loop.dump(llvm::dbgs(), System);
      llvm::dbgs() << "\n";
    }

    for (const auto &pair : result) {
      const auto *proto = pair.first;
      const auto &inEmptyContext = pair.second.RulesInEmptyContext;
      const auto &inContext = pair.second.RulesInContext;
      bool sawIdentityConformance = pair.second.SawIdentityConformance;

      // No rules appear without context.
      if (inEmptyContext.empty())
        continue;

      if (Debug.contains(DebugFlags::MinimalConformances)) {
        llvm::dbgs() << "* Protocol " << proto->getName() << ":\n";
        llvm::dbgs() << "** Conformance rules not in context:\n";
        for (unsigned ruleID : inEmptyContext) {
          llvm::dbgs() << "-- (#" << ruleID << ") " << System.getRule(ruleID) << "\n";
        }

        llvm::dbgs() << "** Conformance rules in context:\n";
        for (auto pair : inContext) {
          llvm::dbgs() << "-- " << pair.first;
          unsigned ruleID = pair.second;
          llvm::dbgs() << " (#" << ruleID << ") " << System.getRule(ruleID) << "\n";
        }

        if (sawIdentityConformance) {
          llvm::dbgs() << "** Equivalent to identity conformance\n";
        }

        llvm::dbgs() << "\n";
      }

      // Two conformance rules in empty context (T.[P] => T) and (T'.[P] => T)
      // are interchangeable, and contribute a trivial pair of conformance
      // equations expressing that each one can be written in terms of the
      // other:
      //
      //   (T.[P] => T) := (T'.[P])
      //   (T'.[P] => T') := (T.[P])
      for (unsigned candidateRuleID : inEmptyContext) {
        for (unsigned otherRuleID : inEmptyContext) {
          if (otherRuleID == candidateRuleID)
            continue;

          SmallVector<unsigned, 2> path;
          path.push_back(otherRuleID);
          ConformancePaths[candidateRuleID].push_back(path);
        }
      }

      // If a rewrite loop contains a conformance rule (T.[P] => T) together
      // with the identity conformance ([P].[P] => [P]), both in empty context,
      // the conformance rule (T.[P] => T) is equivalent to the *empty product*
      // of conformance rules; that is, it is trivially redundant.
      if (sawIdentityConformance) {
        for (unsigned candidateRuleID : inEmptyContext) {
          SmallVector<unsigned, 2> emptyPath;
          ConformancePaths[candidateRuleID].push_back(emptyPath);
        }
      }

      // Suppose a rewrite loop contains a conformance rule (T.[P] => T) in
      // empty context, and a conformance rule (V.[P] => V) in non-empty left
      // context U.
      //
      // The rewrite loop looks something like this:
      //
      //     ... ⊗ (T.[P] => T) ⊗ ... ⊗ U.(V => V.[P]) ⊗ ...
      //    ^                                               ^
      //    |                                               |
      //    + basepoint ========================= basepoint +
      //
      // We can decompose U into a product of conformance rules:
      //
      //    (V1.[P1] => V1)...(Vn.[Pn] => Vn),
      //
      // Note that (V1)...(Vn) is canonically equivalent to U.
      //
      // Now, we can record a candidate decomposition of (T.[P] => T) as a
      // product of conformance rules:
      //
      //    (T.[P] => T) := (V1.[P1] => V1)...(Vn.[Pn] => Vn).(V.[P] => V)
      //
      // Again, note that (V1)...(Vn).V is canonically equivalent to U.V,
      // and therefore T.
      for (auto pair : inContext) {
        // We have a term U, and a rule V.[P] => V.
        SmallVector<unsigned, 2> conformancePath;

        // Simplify U to get U'.
        MutableTerm term = pair.first;
        (void) System.simplify(term);

        // Write U'.[domain(V)] as a product of left hand sides of protocol
        // conformance rules.
        decomposeTermIntoConformanceRuleLeftHandSides(term, pair.second,
                                                      conformancePath);

        // This decomposition defines a conformance access path for each
        // conformance rule we saw in empty context.
        for (unsigned otherRuleID : inEmptyContext)
          ConformancePaths[otherRuleID].push_back(conformancePath);
      }
    }
  }

  for (const auto &pair : ConformancePaths) {
    if (pair.second.size() > 1)
      Context.MinimalConformancesHistogram.add(pair.second.size());
  }
}

/// If \p ruleID is redundant, determines if it can be expressed without
/// without any of the conformance rules determined to be redundant so far.
///
/// If the rule is not redundant, determines if its parent path can
/// also be recovered.
bool MinimalConformances::isConformanceRuleRecoverable(
    llvm::SmallDenseSet<unsigned, 4> &visited,
    unsigned ruleID) const {
  if (RedundantConformances.count(ruleID)) {
    SWIFT_DEFER {
      visited.erase(ruleID);
    };
    visited.insert(ruleID);

    auto found = ConformancePaths.find(ruleID);
    if (found == ConformancePaths.end())
      return false;

    bool foundValidConformancePath = false;
    for (const auto &otherPath : found->second) {
      if (isValidConformancePath(visited, otherPath)) {
        foundValidConformancePath = true;
        break;
      }
    }

    if (!foundValidConformancePath)
      return false;
  } else {
    auto found = ParentPaths.find(ruleID);
    if (found != ParentPaths.end()) {
      SWIFT_DEFER {
        visited.erase(ruleID);
      };
      visited.insert(ruleID);

      // If 'req' is based on some other conformance requirement
      // `T.[P.]A : Q', we want to make sure that we have a
      // non-redundant derivation for 'T : P'.
      if (!isValidConformancePath(visited, found->second))
        return false;
    }
  }

  return true;
}

/// Determines if \p path can be expressed without any of the conformance
/// rules determined to be redundant so far, by possibly substituting
/// any occurrences of the redundant rules with alternate definitions
/// appearing in the set of known conformancePaths.
///
/// The conformance path map sends conformance rules to a list of
/// disjunctions, where each disjunction is a product of other conformance
/// rules.
bool MinimalConformances::isValidConformancePath(
    llvm::SmallDenseSet<unsigned, 4> &visited,
    const llvm::SmallVectorImpl<unsigned> &path) const {

  for (unsigned ruleID : path) {
    if (visited.count(ruleID) > 0)
      return false;

    if (!isConformanceRuleRecoverable(visited, ruleID))
      return false;
  }

  return true;
}

/// Rules of the form [P].[Q] => [P] encode protocol refinement and can only
/// be redundant if they're equivalent to a sequence of other protocol
/// refinements.
///
/// This helps ensure that the inheritance clause of a protocol is complete
/// and correct, allowing name lookup to find associated types of inherited
/// protocols while building the protocol requirement signature.
bool MinimalConformances::isValidRefinementPath(
    const llvm::SmallVectorImpl<unsigned> &path) const {
  for (unsigned ruleID : path) {
    if (ProtocolRefinements.count(ruleID) == 0)
      return false;
  }

  return true;
}

void MinimalConformances::dumpMinimalConformanceEquations(
    llvm::raw_ostream &out) const {
  out << "Initial set of equations:\n";
  for (const auto &pair : ConformancePaths) {
    out << "- ";
    dumpMinimalConformanceEquation(out, pair.first, pair.second);
    out << "\n";
  }

  out << "Parent paths:\n";
  for (const auto &pair : ParentPaths) {
    out << "- " << System.getRule(pair.first).getLHS() << ": ";
    dumpConformancePath(out, pair.second);
    out << "\n";
  }
}

void MinimalConformances::dumpConformancePath(
    llvm::raw_ostream &out,
    const SmallVectorImpl<unsigned> &path) const {
  if (path.empty()) {
    out << "1";
    return;
  }

  for (unsigned ruleID : path)
    out << "(" << System.getRule(ruleID).getLHS() << ")";
}

void MinimalConformances::dumpMinimalConformanceEquation(
    llvm::raw_ostream &out,
    unsigned baseRuleID,
    const std::vector<SmallVector<unsigned, 2>> &paths) const {
  out << System.getRule(baseRuleID).getLHS() << " := ";

  bool first = true;
  for (const auto &path : paths) {
    if (!first)
      out << " ∨ ";
    else
      first = false;

    dumpConformancePath(out, path);
  }
}

void MinimalConformances::verifyMinimalConformanceEquations() const {
  for (const auto &pair : ConformancePaths) {
    const auto &rule = System.getRule(pair.first);
    auto *proto = rule.getLHS().back().getProtocol();

    MutableTerm baseTerm(rule.getRHS());
    (void) System.simplify(baseTerm);

    for (const auto &path : pair.second) {
      if (path.empty())
        continue;

      const auto &otherRule = System.getRule(path.back());
      auto *otherProto = otherRule.getLHS().back().getProtocol();

      if (proto != otherProto) {
        llvm::errs() << "Invalid equation: ";
        dumpMinimalConformanceEquation(llvm::errs(),
                                       pair.first, pair.second);
        llvm::errs() << "\n";
        llvm::errs() << "Mismatched conformance:\n";
        llvm::errs() << "Base rule: " << rule << "\n";
        llvm::errs() << "Final rule: " << otherRule << "\n\n";
        dumpMinimalConformanceEquations(llvm::errs());
        abort();
      }

      MutableTerm otherTerm;
      for (unsigned i : indices(path)) {
        unsigned otherRuleID = path[i];
        const auto &rule = System.getRule(otherRuleID);

        bool isLastElement = (i == path.size() - 1);
        if ((isLastElement && !rule.isAnyConformanceRule()) ||
            (!isLastElement && !rule.isProtocolConformanceRule())) {
          llvm::errs() << "Equation term is not a conformance rule: ";
          dumpMinimalConformanceEquation(llvm::errs(),
                                         pair.first, pair.second);
          llvm::errs() << "\n";
          llvm::errs() << "Term: " << rule << "\n";
          dumpMinimalConformanceEquations(llvm::errs());
          abort();
        }

        otherTerm.append(rule.getRHS());
      }

      (void) System.simplify(otherTerm);

      if (baseTerm != otherTerm) {
        llvm::errs() << "Invalid equation: ";
        dumpMinimalConformanceEquation(llvm::errs(),
                                       pair.first, pair.second);
        llvm::errs() << "\n";
        llvm::errs() << "Invalid conformance path:\n";
        llvm::errs() << "Expected: " << baseTerm << "\n";
        llvm::errs() << "Got: " << otherTerm << "\n\n";
        dumpMinimalConformanceEquations(llvm::errs());
        abort();
      }
    }
  }
}

bool MinimalConformances::isDerivedViaCircularConformanceRule(
    const std::vector<SmallVector<unsigned, 2>> &paths) const {
  for (const auto &path : paths) {
    if (!path.empty() &&
        System.getRule(path.back()).isCircularConformanceRule())
      return true;
  }

  return false;
}

/// Find a set of minimal conformances by marking all non-minimal
/// conformances redundant.
///
/// In the first pass, we only consider conformance requirements that are
/// made redundant by concrete conformances.
void MinimalConformances::computeMinimalConformances() {
  // First, mark any concrete conformances derived via a circular
  // conformance as redundant upfront. See the comment at the top of
  // Rule::isCircularConformanceRule() for an explanation of this.
  for (unsigned ruleID : ConformanceRules) {
    auto found = ConformancePaths.find(ruleID);
    if (found == ConformancePaths.end())
      continue;

    const auto &paths = found->second;

    if (System.getRule(ruleID).isProtocolConformanceRule())
      continue;

    if (isDerivedViaCircularConformanceRule(paths))
      RedundantConformances.insert(ruleID);
  }

  // Now, visit each conformance rule, trying to make it redundant by
  // deriving a path in terms of other non-redundant conformance rules.
  //
  // Note that the ConformanceRules vector is sorted in descending
  // canonical term order, so less canonical rules are eliminated first.
  for (unsigned ruleID : ConformanceRules) {
    auto found = ConformancePaths.find(ruleID);
    if (found == ConformancePaths.end())
      continue;

    const auto &rule = System.getRule(ruleID);
    const auto &paths = found->second;

    bool isProtocolRefinement = ProtocolRefinements.count(ruleID) > 0;

    for (const auto &path : paths) {
      // Only consider a protocol refinement rule to be redundant if it is
      // witnessed by a composition of other protocol refinement rules.
      if (isProtocolRefinement && !isValidRefinementPath(path)) {
        if (Debug.contains(DebugFlags::MinimalConformances)) {
          llvm::dbgs() << "Not a refinement path: ";
          dumpConformancePath(llvm::errs(), path);
          llvm::dbgs() << "\n";
        }
        continue;
      }

      llvm::SmallDenseSet<unsigned, 4> visited;
      visited.insert(ruleID);

      if (isValidConformancePath(visited, path)) {
        if (Debug.contains(DebugFlags::MinimalConformances)) {
          llvm::dbgs() << "Redundant rule: ";
          llvm::dbgs() << rule.getLHS();
          llvm::dbgs() << "\n";
          llvm::dbgs() << "-- via valid path: ";
          dumpConformancePath(llvm::errs(), path);
          llvm::dbgs() << "\n";
        }

        RedundantConformances.insert(ruleID);
        break;
      }
    }
  }
}

/// Check invariants.
void MinimalConformances::verifyMinimalConformances() const {
  for (const auto &pair : ConformancePaths) {
    unsigned ruleID = pair.first;
    const auto &rule = System.getRule(ruleID);

    if (RedundantConformances.count(ruleID) > 0) {
      // Check that redundant conformances are recoverable via
      // minimal conformances.
      llvm::SmallDenseSet<unsigned, 4> visited;

      if (!isConformanceRuleRecoverable(visited, ruleID)) {
        llvm::errs() << "Redundant conformance is not recoverable:\n";
        llvm::errs() << rule << "\n\n";
        dumpMinimalConformanceEquations(llvm::errs());
        dumpMinimalConformances(llvm::errs());
        abort();
      }

      continue;
    }

    if (rule.containsUnresolvedSymbols()) {
      llvm::errs() << "Minimal conformance contains unresolved symbols: ";
      llvm::errs() << rule << "\n\n";
      dumpMinimalConformanceEquations(llvm::errs());
      dumpMinimalConformances(llvm::errs());
      abort();
    }
  }
}

void MinimalConformances::dumpMinimalConformances(
    llvm::raw_ostream &out) const {
  out << "Minimal conformances:\n";

  for (unsigned ruleID : ConformanceRules) {
    if (RedundantConformances.count(ruleID) > 0)
      continue;

    out << "- " << System.getRule(ruleID) << "\n";
  }
}

/// Computes minimal conformances, assuming that homotopy reduction has
/// already eliminated all redundant rewrite rules that are not
/// conformance rules.
void RewriteSystem::computeMinimalConformances(
    llvm::DenseSet<unsigned> &redundantConformances) {
  MinimalConformances builder(*this, redundantConformances);

  builder.collectConformanceRules();
  builder.computeCandidateConformancePaths();

  if (Debug.contains(DebugFlags::MinimalConformances)) {
    builder.dumpMinimalConformanceEquations(llvm::dbgs());
  }

  builder.verifyMinimalConformanceEquations();
  builder.computeMinimalConformances();
  builder.verifyMinimalConformances();

  if (Debug.contains(DebugFlags::MinimalConformances)) {
    builder.dumpMinimalConformances(llvm::dbgs());
  }
}
