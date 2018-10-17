/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Robin Suerig, Philipp Schubert and others
 *****************************************************************************/

#ifndef PHASAR_PHASARLLVM_IFDSIDE_PROBLEMS_IDETYPESTATEANALYSIS_H_
#define PHASAR_PHASARLLVM_IFDSIDE_PROBLEMS_IDETYPESTATEANALYSIS_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <phasar/PhasarLLVM/IfdsIde/DefaultIDETabulationProblem.h>
#include <phasar/PhasarLLVM/IfdsIde/EdgeFunctionComposer.h>       //new

namespace llvm {
class Instruction;
class Function;
class Value;
} // namespace llvm

namespace psr {
class LLVMBasedICFG;

enum State { uninit = 0, opened, closed, error };

class IDETypeStateAnalysis
    : public DefaultIDETabulationProblem<
          const llvm::Instruction *, const llvm::Value *,
          const llvm::Function *, State, LLVMBasedICFG &> {
public:
  typedef const llvm::Value *d_t;
  typedef const llvm::Instruction *n_t;
  typedef const llvm::Function *m_t;
  typedef State v_t;
  typedef LLVMBasedICFG &i_t;
  
private:
  std::vector<std::string> EntryPoints;
  static const std::set<std::string> STDIOFunctions;
  static const std::shared_ptr<EdgeFunction<v_t>> AllBot;

public:

  static const State TOP;
  static const State BOTTOM;

  IDETypeStateAnalysis(i_t icfg,
                       std::vector<std::string> EntryPoints = {"main"});

  virtual ~IDETypeStateAnalysis() = default;

  // start formulating our analysis by specifying the parts required for IFDS

  std::shared_ptr<FlowFunction<d_t>> getNormalFlowFunction(n_t curr,
                                                           n_t succ) override;

  std::shared_ptr<FlowFunction<d_t>> getCallFlowFunction(n_t callStmt,
                                                         m_t destMthd) override;

  std::shared_ptr<FlowFunction<d_t>> getRetFlowFunction(n_t callSite,
                                                        m_t calleeMthd,
                                                        n_t exitStmt,
                                                        n_t retSite) override;

  std::shared_ptr<FlowFunction<d_t>>
  getCallToRetFlowFunction(n_t callSite, n_t retSite,
                           std::set<m_t> callees) override;

  std::shared_ptr<FlowFunction<d_t>>
  getSummaryFlowFunction(n_t callStmt, m_t destMthd) override;

  std::map<n_t, std::set<d_t>> initialSeeds() override;

  d_t createZeroValue() override;

  bool isZeroValue(d_t d) const override;

  // in addition provide specifications for the IDE parts

  std::shared_ptr<EdgeFunction<v_t>>
  getNormalEdgeFunction(n_t curr, d_t currNode, n_t succ,
                        d_t succNode) override;

  std::shared_ptr<EdgeFunction<v_t>> getCallEdgeFunction(n_t callStmt,
                                                         d_t srcNode,
                                                         m_t destiantionMethod,
                                                         d_t destNode) override;

  std::shared_ptr<EdgeFunction<v_t>>
  getReturnEdgeFunction(n_t callSite, m_t calleeMethod, n_t exitStmt,
                        d_t exitNode, n_t reSite, d_t retNode) override;

  std::shared_ptr<EdgeFunction<v_t>>
  getCallToRetEdgeFunction(n_t callSite, d_t callNode, n_t retSite,
                           d_t retSiteNode, std::set<m_t> callees) override;

  std::shared_ptr<EdgeFunction<v_t>>
  getSummaryEdgeFunction(n_t callStmt, d_t callNode, n_t retSite,
                         d_t retSiteNode) override;

  v_t topElement() override;

  v_t bottomElement() override;

  v_t join(v_t lhs, v_t rhs) override;

  std::shared_ptr<EdgeFunction<v_t>> allTopFunction() override;

  enum class State { uninit = 0, opened, closed, error };

  enum class CurrentState {
    state_fopen = 0,
    state_fclose,
    state_star,
    state_freopen
  };

  void printNode(std::ostream &os, n_t d) const override;

  void printDataFlowFact(std::ostream &os, d_t d) const override;

  void printMethod(std::ostream &os, m_t m) const override;

  void printValue(std::ostream &os, v_t v) const override;
};

} // namespace psr

#endif
