/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation,
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 * @file    QPSolver.cpp
 * @brief
 * @date    Apr 15, 2014
 * @author  Duy-Nguyen Ta
 */

#include <gtsam/inference/Symbol.h>
#include <gtsam/inference/FactorGraph-inst.h>
#include <gtsam_unstable/linear/QPSolver.h>

#include <boost/range/adaptor/map.hpp>

using namespace std;

namespace gtsam {

//******************************************************************************
QPSolver::QPSolver(const QP& qp) : qp_(qp) {
  baseGraph_ = qp_.cost;
  baseGraph_.push_back(qp_.equalities.begin(), qp_.equalities.end());
  costVariableIndex_ = VariableIndex(qp_.cost);
  equalityVariableIndex_ = VariableIndex(qp_.equalities);
  inequalityVariableIndex_ = VariableIndex(qp_.inequalities);
  constrainedKeys_ = qp_.equalities.keys();
  constrainedKeys_.merge(qp_.inequalities.keys());
}

//******************************************************************************
VectorValues QPSolver::solveWithCurrentWorkingSet(
    const InequalityFactorGraph& workingSet) const {
  GaussianFactorGraph workingGraph = baseGraph_;
  BOOST_FOREACH(const LinearInequality::shared_ptr& factor, workingSet) {
    if (factor->active())
      workingGraph.push_back(factor);
  }
  return workingGraph.optimize();
}

//******************************************************************************
JacobianFactor::shared_ptr QPSolver::createDualFactor(Key key,
    const InequalityFactorGraph& workingSet, const VectorValues& delta) const {

  // Transpose the A matrix of constrained factors to have the jacobian of the dual key
  std::vector<std::pair<Key, Matrix> > Aterms = collectDualJacobians
      < LinearEquality > (key, qp_.equalities, equalityVariableIndex_);
  std::vector<std::pair<Key, Matrix> > AtermsInequalities = collectDualJacobians
      < LinearInequality > (key, workingSet, inequalityVariableIndex_);
  Aterms.insert(Aterms.end(), AtermsInequalities.begin(),
      AtermsInequalities.end());

  // Collect the gradients of unconstrained cost factors to the b vector
  if (Aterms.size() > 0) {
    Vector b = zero(delta.at(key).size());
    if (costVariableIndex_.find(key) != costVariableIndex_.end()) {
      BOOST_FOREACH(size_t factorIx, costVariableIndex_[key]) {
        GaussianFactor::shared_ptr factor = qp_.cost.at(factorIx);
        b += factor->gradient(key, delta);
      }
    }
    return boost::make_shared<JacobianFactor>(Aterms, b);   // compute the least-square approximation of dual variables
  }
  else {
    return boost::make_shared<JacobianFactor>();
  }
}

//******************************************************************************
GaussianFactorGraph::shared_ptr QPSolver::buildDualGraph(
    const InequalityFactorGraph& workingSet, const VectorValues& delta) const {
  GaussianFactorGraph::shared_ptr dualGraph(new GaussianFactorGraph());
  BOOST_FOREACH(Key key, constrainedKeys_) {
    // Each constrained key becomes a factor in the dual graph
    JacobianFactor::shared_ptr dualFactor = createDualFactor(key, workingSet, delta);
    if (!dualFactor->empty())
      dualGraph->push_back(dualFactor);
  }
  return dualGraph;
}

//******************************************************************************
int QPSolver::identifyLeavingConstraint(
    const InequalityFactorGraph& workingSet,
    const VectorValues& lambdas) const {
  int worstFactorIx = -1;
  // preset the maxLambda to 0.0: if lambda is <= 0.0, the constraint is either
  // inactive or a good inequality constraint, so we don't care!
  double maxLambda = 0.0;
  for (size_t factorIx = 0; factorIx < workingSet.size(); ++factorIx) {
    const LinearInequality::shared_ptr& factor = workingSet.at(factorIx);
    if (factor->active()) {
      double lambda = lambdas.at(factor->dualKey())[0];
      if (lambda > maxLambda) {
        worstFactorIx = factorIx;
        maxLambda = lambda;
      }
    }
  }
  return worstFactorIx;
}

//******************************************************************************
/* We have to make sure the new solution with alpha satisfies all INACTIVE inequality constraints
 * If some inactive inequality constraints complain about the full step (alpha = 1),
 * we have to adjust alpha to stay within the inequality constraints' feasible regions.
 *
 * For each inactive inequality j:
 *  - We already have: aj'*xk - bj <= 0, since xk satisfies all inequality constraints
 *  - We want: aj'*(xk + alpha*p) - bj <= 0
 *  - If aj'*p <= 0, we have: aj'*(xk + alpha*p) <= aj'*xk <= bj, for all alpha>0
 *  it's good!
 *  - We only care when aj'*p > 0. In this case, we need to choose alpha so that
 *  aj'*xk + alpha*aj'*p - bj <= 0  --> alpha <= (bj - aj'*xk) / (aj'*p)
 *  We want to step as far as possible, so we should choose alpha = (bj - aj'*xk) / (aj'*p)
 *
 * We want the minimum of all those alphas among all inactive inequality.
 */
boost::tuple<double, int> QPSolver::computeStepSize(
    const InequalityFactorGraph& workingSet, const VectorValues& xk,
    const VectorValues& p) const {
  static bool debug = false;

  double minAlpha = 1.0;
  int closestFactorIx = -1;
  for(size_t factorIx = 0; factorIx<workingSet.size(); ++factorIx) {
    const LinearInequality::shared_ptr& factor = workingSet.at(factorIx);
    double b = factor->getb()[0];
    // only check inactive factors
    if (!factor->active()) {
      // Compute a'*p
      double aTp = factor->dotProductRow(p);

      // Check if  a'*p >0. Don't care if it's not.
      if (aTp <= 0)
        continue;

      // Compute a'*xk
      double aTx = factor->dotProductRow(xk);

      // alpha = (b - a'*xk) / (a'*p)
      double alpha = (b - aTx) / aTp;
      if (debug)
        cout << "alpha: " << alpha << endl;

      // We want the minimum of all those max alphas
      if (alpha < minAlpha) {
        closestFactorIx = factorIx;
        minAlpha = alpha;
      }
    }

  }

  return boost::make_tuple(minAlpha, closestFactorIx);
}

//******************************************************************************
QPState QPSolver::iterate(const QPState& state) const {
  static bool debug = false;

  // Algorithm 16.3 from Nocedal06book.
  // Solve with the current working set eqn 16.39, but instead of solving for p solve for x
  VectorValues newValues = solveWithCurrentWorkingSet(state.workingSet);
  if (debug)
    newValues.print("New solution:");

  // If we CAN'T move further
  // if p_k = 0 is the original condition, modified by Duy to say that the state update is zero.
  if (newValues.equals(state.values, 1e-7)) {
    // Compute lambda from the dual graph
    if (debug)
      cout << "Building dual graph..." << endl;
    GaussianFactorGraph::shared_ptr dualGraph = buildDualGraph(state.workingSet, newValues);
    if (debug)
      dualGraph->print("Dual graph: ");
    VectorValues duals = dualGraph->optimize();
    if (debug)
      duals.print("Duals :");

    int leavingFactor = identifyLeavingConstraint(state.workingSet, duals);
    if (debug)
      cout << "leavingFactor: " << leavingFactor << endl;

    // If all inequality constraints are satisfied: We have the solution!!
    if (leavingFactor < 0) {
      return QPState(newValues, duals, state.workingSet, true, state.iterations+1);
    }
    else {
      // Inactivate the leaving constraint
      InequalityFactorGraph newWorkingSet = state.workingSet;
      newWorkingSet.at(leavingFactor)->inactivate();
      return QPState(newValues, duals, newWorkingSet, false, state.iterations+1);
    }
  }
  else {
    // If we CAN make some progress, i.e. p_k != 0
    // Adapt stepsize if some inactive constraints complain about this move
    double alpha;
    int factorIx;
    VectorValues p = newValues - state.values;
    boost::tie(alpha, factorIx) = // using 16.41
        computeStepSize(state.workingSet, state.values, p);
    if (debug)
      cout << "alpha, factorIx: " << alpha << " " << factorIx << " "
           << endl;

    // also add to the working set the one that complains the most
    InequalityFactorGraph newWorkingSet = state.workingSet;
    if (factorIx >= 0)
      newWorkingSet.at(factorIx)->activate();

    // step!
    newValues = state.values + alpha * p;

    return QPState(newValues, state.duals, newWorkingSet, false, state.iterations+1);
  }
}

//******************************************************************************
InequalityFactorGraph QPSolver::identifyActiveConstraints(
    const InequalityFactorGraph& inequalities,
    const VectorValues& initialValues, const VectorValues& duals, bool useWarmStart) const {
  InequalityFactorGraph workingSet;
  BOOST_FOREACH(const LinearInequality::shared_ptr& factor, inequalities) {
    LinearInequality::shared_ptr workingFactor(new LinearInequality(*factor));
    if (useWarmStart == true && duals.exists(workingFactor->dualKey())) {
      workingFactor->activate();
    }
    else {
      if (useWarmStart == true && duals.size() > 0) {
        workingFactor->inactivate();
      } else {
        double error = workingFactor->error(initialValues);
        // TODO: find a feasible initial point for QPSolver.
        // For now, we just throw an exception, since we don't have an LPSolver to do this yet
        if (error > 0)
          throw InfeasibleInitialValues();

        if (fabs(error)<1e-7) {
          workingFactor->activate();
        }
        else {
          workingFactor->inactivate();
        }
      }
    }
    workingSet.push_back(workingFactor);
  }
  return workingSet;
}

//******************************************************************************
pair<VectorValues, VectorValues> QPSolver::optimize(
    const VectorValues& initialValues, const VectorValues& duals, bool useWarmStart) const {

  // Initialize workingSet from the feasible initialValues
  InequalityFactorGraph workingSet =
      identifyActiveConstraints(qp_.inequalities, initialValues, duals, useWarmStart);
  QPState state(initialValues, duals, workingSet, false, 0);

  /// main loop of the solver
  while (!state.converged) {
    state = iterate(state);
  }

  return make_pair(state.values, state.duals);
}

} /* namespace gtsam */