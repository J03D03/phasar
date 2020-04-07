/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

/*
 * PointsToGraph.cpp
 *
 *  Created on: 08.02.2017
 *      Author: pdschbrt
 */
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include "boost/graph/copy.hpp"
#include "boost/graph/depth_first_search.hpp"
#include "boost/graph/graph_utility.hpp"
#include "boost/graph/graphviz.hpp"
#include "boost/log/sources/record_ostream.hpp"

#include "phasar/PhasarLLVM/Pointer/LLVMPointsToGraph.h"

#include "phasar/Utils/GraphExtensions.h"
#include "phasar/Utils/LLVMShorthands.h"
#include "phasar/Utils/Logger.h"
#include "phasar/Utils/PAMMMacros.h"
#include "phasar/Utils/Utilities.h"

using namespace std;
using namespace psr;

namespace psr {
struct PointsToGraph::AllocationSiteDFSVisitor : boost::default_dfs_visitor {
  // collect the allocation sites that are found
  std::set<const llvm::Value *> &AllocationSites;
  // keeps track of the current path
  std::vector<vertex_t> VisitorStack;
  // the call stack that can be matched against the visitor stack
  const std::vector<const llvm::Instruction *> &CallStack;

  AllocationSiteDFSVisitor(std::set<const llvm::Value *> &AllocationSizes,
                           const vector<const llvm::Instruction *> &CallStack)
      : AllocationSites(AllocationSizes), CallStack(CallStack) {}

  template <typename Vertex, typename Graph>
  void discover_vertex(Vertex U, const Graph &G) {
    VisitorStack.push_back(U);
  }

  template <typename Vertex, typename Graph>
  void finish_vertex(Vertex U, const Graph &G) {
    auto &LG = lg::get();
    // check for stack allocation
    if (const llvm::AllocaInst *Alloc =
            llvm::dyn_cast<llvm::AllocaInst>(G[U].V)) {
      // If the call stack is empty, we completely ignore the calling context
      if (matches_stack(G) || CallStack.empty()) {
        LOG_IF_ENABLE(BOOST_LOG_SEV(LG, DEBUG)
                      << "Found stack allocation: " << llvmIRToString(Alloc));
        AllocationSites.insert(G[U].V);
      }
    }
    // check for heap allocation
    if (llvm::isa<llvm::CallInst>(G[U].V) ||
        llvm::isa<llvm::InvokeInst>(G[U].V)) {
      llvm::ImmutableCallSite CS(G[U].V);
      if (CS.getCalledFunction() != nullptr &&
          HeapAllocationFunctions.count(
              CS.getCalledFunction()->getName().str())) {
        // If the call stack is empty, we completely ignore the calling
        // context
        if (matches_stack(G) || CallStack.empty()) {
          LOG_IF_ENABLE(BOOST_LOG_SEV(LG, DEBUG)
                        << "Found heap allocation: "
                        << llvmIRToString(CS.getInstruction()));
          AllocationSites.insert(G[U].V);
        }
      }
    }
    VisitorStack.pop_back();
  }

  template <typename Graph> bool matches_stack(const Graph &G) {
    size_t CallStackIdx = 0;
    for (size_t I = 0, J = 1;
         I < VisitorStack.size() && J < VisitorStack.size(); ++I, ++J) {
      auto E = boost::edge(VisitorStack[I], VisitorStack[J], G);
      if (G[E.first].V == nullptr)
        continue;
      if (G[E.first].V != CallStack[CallStack.size() - CallStackIdx - 1]) {
        return false;
      }
      CallStackIdx++;
    }
    return true;
  }
};

struct PointsToGraph::ReachabilityDFSVisitor : boost::default_dfs_visitor {
  std::set<vertex_t> &PointsToSet;
  ReachabilityDFSVisitor(set<vertex_t> &Result) : PointsToSet(Result) {}
  template <typename Vertex, typename Graph>
  void finish_vertex(Vertex U, const Graph &G) {
    PointsToSet.insert(U);
  }
};

// points-to graph internal stuff

PointsToGraph::VertexProperties::VertexProperties(const llvm::Value *V)
    : V(V) {}

std::string PointsToGraph::VertexProperties::getValueAsString() const {
  return llvmIRToString(V);
}

std::vector<const llvm::User *>
PointsToGraph::VertexProperties::getUsers() const {
  if (!users.empty() || V == nullptr) {
    return users;
  }
  auto AllUsers = V->users();
  users.insert(users.end(), AllUsers.begin(), AllUsers.end());
  return users;
}

PointsToGraph::EdgeProperties::EdgeProperties(const llvm::Value *V) : V(V) {}

std::string PointsToGraph::EdgeProperties::getValueAsString() const {
  return llvmIRToString(V);
}

// points-to graph stuff

PointsToGraph::PointsToGraph(llvm::Function *F, llvm::AAResults &AA) {
  PAMM_GET_INSTANCE;
  auto &LG = lg::get();
  LOG_IF_ENABLE(BOOST_LOG_SEV(LG, DEBUG)
                << "Analyzing function: " << F->getName().str());
  ContainedFunctions.insert(F);
  bool PrintAll, PrintNoAlias, PrintMayAlias, PrintPartialAlias, PrintMustAlias,
      EvalAAMD, PrintNoModRef, PrintMod, PrintRef, PrintModRef, PrintMust,
      PrintMustMod, PrintMustRef, PrintMustModRef;
  PrintAll = PrintNoAlias = PrintMayAlias = PrintPartialAlias = PrintMustAlias =
      EvalAAMD = PrintNoModRef = PrintMod = PrintRef = PrintModRef = PrintMust =
          PrintMustMod = PrintMustRef = PrintMustModRef = true;

  // taken from llvm/Analysis/AliasAnalysisEvaluator.cpp
  const llvm::DataLayout &DL = F->getParent()->getDataLayout();

  llvm::SetVector<llvm::Value *> Pointers;
  llvm::SmallSetVector<llvm::CallBase *, 16> Calls;
  llvm::SetVector<llvm::Value *> Loads;
  llvm::SetVector<llvm::Value *> Stores;

  for (auto &I : F->args()) {
    if (I.getType()->isPointerTy()) { // Add all pointer arguments.
      Pointers.insert(&I);
    }
  }

  for (llvm::inst_iterator I = inst_begin(*F), E = inst_end(*F); I != E; ++I) {
    if (I->getType()->isPointerTy()) // Add all pointer instructions.
      Pointers.insert(&*I);
    if (EvalAAMD && llvm::isa<llvm::LoadInst>(&*I))
      Loads.insert(&*I);
    if (EvalAAMD && llvm::isa<llvm::StoreInst>(&*I))
      Stores.insert(&*I);
    llvm::Instruction &Inst = *I;
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
      llvm::Value *Callee = Call->getCalledValue();
      // Skip actual functions for direct function calls.
      if (!llvm::isa<llvm::Function>(Callee) && isInterestingPointer(Callee))
        Pointers.insert(Callee);
      // Consider formals.
      for (llvm::Use &DataOp : Call->data_ops())
        if (isInterestingPointer(DataOp))
          Pointers.insert(DataOp);
      Calls.insert(Call);
    } else {
      // Consider all operands.
      for (llvm::Instruction::op_iterator OI = Inst.op_begin(),
                                          OE = Inst.op_end();
           OI != OE; ++OI)
        if (isInterestingPointer(*OI))
          Pointers.insert(*OI);
    }
  }

  INC_COUNTER("GS Pointer", Pointers.size(), PAMM_SEVERITY_LEVEL::Core);

  // make vertices for all pointers
  for (auto P : Pointers) {
    ValueVertexMap[P] = boost::add_vertex(VertexProperties(P), PAG);
  }
  // iterate over the worklist, and run the full (n^2)/2 disambiguations
  const auto MapEnd = ValueVertexMap.end();
  for (auto I1 = ValueVertexMap.begin(); I1 != MapEnd; ++I1) {
    llvm::Type *I1ElTy =
        llvm::cast<llvm::PointerType>(I1->first->getType())->getElementType();
    const uint64_t I1Size = I1ElTy->isSized()
                                ? DL.getTypeStoreSize(I1ElTy)
                                : llvm::MemoryLocation::UnknownSize;
    for (auto I2 = std::next(I1); I2 != MapEnd; ++I2) {
      llvm::Type *I2ElTy =
          llvm::cast<llvm::PointerType>(I2->first->getType())->getElementType();
      const uint64_t I2Size = I2ElTy->isSized()
                                  ? DL.getTypeStoreSize(I2ElTy)
                                  : llvm::MemoryLocation::UnknownSize;

      switch (AA.alias(I1->first, I1Size, I2->first, I2Size)) {
      case llvm::NoAlias:
        break;
      case llvm::MayAlias:     // no break
      case llvm::PartialAlias: // no break
      case llvm::MustAlias:
        boost::add_edge(I1->second, I2->second, PAG);
        break;
      default:
        break;
      }
    }
  }
}

vector<pair<unsigned, const llvm::Value *>>
PointsToGraph::getPointersEscapingThroughParams() {
  vector<pair<unsigned, const llvm::Value *>> EscapingPointers;
  for (auto VertexIter : boost::make_iterator_range(boost::vertices(PAG))) {
    if (const llvm::Argument *Arg =
            llvm::dyn_cast<llvm::Argument>(PAG[VertexIter].V)) {
      EscapingPointers.push_back(make_pair(Arg->getArgNo(), Arg));
    }
  }
  return EscapingPointers;
}

vector<const llvm::Value *>
PointsToGraph::getPointersEscapingThroughReturns() const {
  vector<const llvm::Value *> EscapingPointers;
  for (auto VertexIter : boost::make_iterator_range(boost::vertices(PAG))) {
    auto &Vertex = PAG[VertexIter];
    for (const auto User : Vertex.getUsers()) {
      if (llvm::isa<llvm::ReturnInst>(User)) {
        EscapingPointers.push_back(Vertex.V);
      }
    }
  }
  return EscapingPointers;
}

vector<const llvm::Value *>
PointsToGraph::getPointersEscapingThroughReturnsForFunction(
    const llvm::Function *F) const {
  vector<const llvm::Value *> EscapingPointers;
  for (auto VertexIter : boost::make_iterator_range(boost::vertices(PAG))) {
    auto &Vertex = PAG[VertexIter];
    for (const auto User : Vertex.getUsers()) {
      if (auto R = llvm::dyn_cast<llvm::ReturnInst>(User)) {
        if (R->getFunction() == F)
          EscapingPointers.push_back(Vertex.V);
      }
    }
  }
  return EscapingPointers;
}

set<const llvm::Value *> PointsToGraph::getReachableAllocationSites(
    const llvm::Value *V, vector<const llvm::Instruction *> CallStack) {
  set<const llvm::Value *> AllocSites;
  AllocationSiteDFSVisitor AllocVis(AllocSites, CallStack);
  vector<boost::default_color_type> ColorMap(boost::num_vertices(PAG));
  boost::depth_first_visit(
      PAG, ValueVertexMap[V], AllocVis,
      boost::make_iterator_property_map(
          ColorMap.begin(), boost::get(boost::vertex_index, PAG), ColorMap[0]));
  return AllocSites;
}

bool PointsToGraph::containsValue(llvm::Value *V) {
  for (auto VertexIter : boost::make_iterator_range(boost::vertices(PAG)))
    if (PAG[VertexIter].V == V)
      return true;
  return false;
}

set<const llvm::Type *>
PointsToGraph::computeTypesFromAllocationSites(set<const llvm::Value *> AS) {
  set<const llvm::Type *> Types;
  // an allocation site can either be an AllocaInst or a call to an allocating
  // function
  for (auto V : AS) {
    if (const llvm::AllocaInst *Alloc = llvm::dyn_cast<llvm::AllocaInst>(V)) {
      Types.insert(Alloc->getAllocatedType());
    } else {
      // usually if an allocating function is called, it is immediately
      // bit-casted
      // to the desired allocated value and hence we can determine it from the
      // destination type of that cast instruction.
      for (auto User : V->users()) {
        if (const llvm::BitCastInst *Cast =
                llvm::dyn_cast<llvm::BitCastInst>(User)) {
          Types.insert(Cast->getDestTy());
        }
      }
    }
  }
  return Types;
}

set<const llvm::Value *>
PointsToGraph::getPointsToSet(const llvm::Value *V) const {
  PAMM_GET_INSTANCE;
  INC_COUNTER("[Calls] getPointsToSet", 1, PAMM_SEVERITY_LEVEL::Full);
  START_TIMER("PointsTo-Set Computation", PAMM_SEVERITY_LEVEL::Full);
  // check if the graph contains a corresponding vertex
  if (!ValueVertexMap.count(V)) {
    return {};
  }
  set<vertex_t> ReachableVertices;
  ReachabilityDFSVisitor Vis(ReachableVertices);
  vector<boost::default_color_type> ColorMap(boost::num_vertices(PAG));
  boost::depth_first_visit(
      PAG, ValueVertexMap.at(V), Vis,
      boost::make_iterator_property_map(
          ColorMap.begin(), boost::get(boost::vertex_index, PAG), ColorMap[0]));
  set<const llvm::Value *> Result;
  for (auto Vertex : ReachableVertices) {
    Result.insert(PAG[Vertex].V);
  }
  PAUSE_TIMER("PointsTo-Set Computation", PAMM_SEVERITY_LEVEL::Full);
  ADD_TO_HISTOGRAM("Points-to", Result.size(), 1, PAMM_SEVERITY_LEVEL::Full);
  return Result;
}

bool PointsToGraph::representsSingleFunction() {
  return ContainedFunctions.size() == 1;
}

void PointsToGraph::print(std::ostream &OS) const {
  for (const auto &Fn : ContainedFunctions) {
    cout << "PointsToGraph for " << Fn->getName().str() << ":\n";
    vertex_iterator UI, UIEnd;
    for (boost::tie(UI, UIEnd) = boost::vertices(PAG); UI != UIEnd; ++UI) {
      OS << PAG[*UI].getValueAsString() << " <--> ";
      out_edge_iterator EI, EIEnd;
      for (boost::tie(EI, EIEnd) = boost::out_edges(*UI, PAG); EI != EIEnd;
           ++EI) {
        OS << PAG[target(*EI, PAG)].getValueAsString() << " ";
      }
      OS << '\n';
    }
  }
}

void PointsToGraph::printAsDot(std::ostream &OS) const {
  boost::write_graphviz(OS, PAG, makePointerVertexOrEdgePrinter(PAG),
                        makePointerVertexOrEdgePrinter(PAG));
}

nlohmann::json PointsToGraph::getAsJson() {
  nlohmann::json J;
  vertex_iterator VIv, VIvEnd;
  out_edge_iterator EI, EIEnd;
  // iterate all graph vertices
  for (boost::tie(VIv, VIvEnd) = boost::vertices(PAG); VIv != VIvEnd; ++VIv) {
    J[PhasarConfig::JsonPointToGraphID()][PAG[*VIv].getValueAsString()];
    // iterate all out edges of vertex vi_v
    for (boost::tie(EI, EIEnd) = boost::out_edges(*VIv, PAG); EI != EIEnd;
         ++EI) {
      J[PhasarConfig::JsonPointToGraphID()][PAG[*VIv].getValueAsString()] +=
          PAG[boost::target(*EI, PAG)].getValueAsString();
    }
  }
  return J;
}

void PointsToGraph::printValueVertexMap() {
  for (const auto &Entry : ValueVertexMap) {
    cout << Entry.first << " <---> " << Entry.second << endl;
  }
}

void PointsToGraph::mergeGraph(const PointsToGraph &Other) {
  typedef graph_t::vertex_descriptor vertex_t;
  typedef std::map<vertex_t, vertex_t> vertex_map_t;
  vertex_map_t OldToNewVertexMapping;
  boost::associative_property_map<vertex_map_t> VertexMapWrapper(
      OldToNewVertexMapping);
  boost::copy_graph(Other.PAG, PAG, boost::orig_to_copy(VertexMapWrapper));

  for (const auto &OtherValues : Other.ValueVertexMap) {
    auto MappingIter = OldToNewVertexMapping.find(OtherValues.second);
    if (MappingIter != OldToNewVertexMapping.end()) {
      ValueVertexMap.insert(make_pair(OtherValues.first, MappingIter->second));
    }
  }
}

void PointsToGraph::mergeCallSite(const llvm::ImmutableCallSite &CS,
                                  const llvm::Function *F) {
  auto FormalArgRange = F->args();
  auto FormalIter = FormalArgRange.begin();
  auto MapEnd = ValueVertexMap.end();
  for (const auto &Arg : CS.args()) {
    const llvm::Argument *Formal = &*FormalIter++;
    auto ArgMapIter = ValueVertexMap.find(Arg);
    auto FormalMapIter = ValueVertexMap.find(Formal);
    if (ArgMapIter != MapEnd && FormalMapIter != MapEnd) {
      boost::add_edge(ArgMapIter->second, FormalMapIter->second,
                      CS.getInstruction(), PAG);
    }
    if (FormalIter == FormalArgRange.end())
      break;
  }

  for (auto Formal : getPointersEscapingThroughReturnsForFunction(F)) {
    auto InstrMapIter = ValueVertexMap.find(CS.getInstruction());
    auto FormalMapIter = ValueVertexMap.find(Formal);
    if (InstrMapIter != MapEnd && FormalMapIter != MapEnd) {
      boost::add_edge(InstrMapIter->second, FormalMapIter->second,
                      CS.getInstruction(), PAG);
    }
  }
}

void PointsToGraph::mergeWith(const PointsToGraph *Other,
                              const llvm::Function *F) {
  if (ContainedFunctions.insert(F).second) {
    mergeGraph(*Other);
  }
}

void PointsToGraph::mergeWith(
    const PointsToGraph &Other,
    const vector<pair<llvm::ImmutableCallSite, const llvm::Function *>>
        &Calls) {
  ContainedFunctions.insert(Other.ContainedFunctions.begin(),
                            Other.ContainedFunctions.end());
  mergeGraph(Other);
  for (const auto &Call : Calls) {
    mergeCallSite(Call.first, Call.second);
  }
}

bool PointsToGraph::empty() const { return size() == 0; }

size_t PointsToGraph::size() const { return getNumVertices(); }

size_t PointsToGraph::getNumVertices() const {
  return boost::num_vertices(PAG);
}

size_t PointsToGraph::getNumEdges() const { return boost::num_edges(PAG); }

} // namespace psr
