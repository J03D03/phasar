#pragma once

#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/EdgeFunction.h"
#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/Problems/IDEGeneralizedLCA/IDEGeneralizedLCA.h"
#include "phasar/PhasarLLVM/DataFlowSolver/IfdsIde/Problems/IDEGeneralizedLCA/EdgeValueSet.h"

namespace psr {

class BinaryEdgeFunction
    : public EdgeFunction<IDEGeneralizedLCA::v_t>,
      public std::enable_shared_from_this<BinaryEdgeFunction> {
  llvm::BinaryOperator::BinaryOps op;
  const IDEGeneralizedLCA::v_t cnst;
  bool leftConst;
  size_t maxSize;

public:
  BinaryEdgeFunction(llvm::BinaryOperator::BinaryOps op,
                     const IDEGeneralizedLCA::v_t &cnst, bool leftConst,
                     size_t maxSize)
      : op(op), cnst(cnst), leftConst(leftConst), maxSize(maxSize) {}

  IDEGeneralizedLCA::v_t computeTarget(IDEGeneralizedLCA::v_t source) override;

  std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> composeWith(
      std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> secondFunction)
      override;

  std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> joinWith(
      std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>> otherFunction)
      override;

  bool equal_to(std::shared_ptr<EdgeFunction<IDEGeneralizedLCA::v_t>>
                    other) const override;

  void print(std::ostream &OS, bool isForDebug = false) const override;
};

} // namespace psr