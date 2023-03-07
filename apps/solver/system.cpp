#include "system.h"

#include <apps/constant.h>
#include <apps/global_preferences.h>
#include <apps/shared/expression_display_permissions.h>
#include <apps/shared/poincare_helpers.h>
#include <poincare/matrix.h>
#include <poincare/polynomial.h>
#include <poincare/symbol.h>

#include "app.h"

using namespace Poincare;
using namespace Shared;

namespace Solver {

System::Error System::exactSolve(Context *context) {
  m_overrideUserVariables = false;
  Error firstError = privateExactSolve(context);
  if (firstError == Error::NoError && m_numberOfSolutions > 0) {
    return firstError;
  }

  m_overrideUserVariables = true;
  Error secondError = privateExactSolve(context);
  if (firstError == Error::NoError && secondError != Error::NoError &&
      secondError != Error::RequireApproximateSolution) {
    /* The system becomes invalid when overriding the user variables: the first
     * solution was better. */
    m_numberOfSolutions = 0;
    return firstError;
  }
  return secondError;
}

void System::approximateSolve(Context *context) {
  assert(m_type == Type::GeneralMonovariable);
  assert(m_numberOfResolutionVariables == 1);

  m_hasMoreSolutions = false;
  Expression undevelopedExpression =
      m_store->modelForRecord(m_store->definedRecordAtIndex(0))
          ->standardForm(context, m_overrideUserVariables,
                         ReductionTarget::SystemForApproximation);
  m_numberOfSolutions = 0;

  assert(m_approximateResolutionMinimum <= m_approximateResolutionMaximum);
  Poincare::Solver<double> solver = PoincareHelpers::Solver(
      m_approximateResolutionMinimum, m_approximateResolutionMaximum,
      m_variables[0], context);
  solver.stretch();

  for (size_t i = 0; i <= k_maxNumberOfApproximateSolutions; i++) {
    double root = solver.nextRoot(undevelopedExpression).x1();
    if (root < m_approximateResolutionMinimum) {
      i--;
      continue;
    } else if (root > m_approximateResolutionMaximum) {
      root = NAN;
    }

    if (i == k_maxNumberOfApproximateSolutions) {
      m_hasMoreSolutions = true;
    } else {
      if (std::isnan(root)) {
        break;
      }
      registerSolution(root);
    }
  }
}

void System::tidy(char *treePoolCursor) {
  for (int i = 0; i < k_maxNumberOfSolutions; i++) {
    if (treePoolCursor == nullptr ||
        m_solutions[i].exactLayout().isDownstreamOf(treePoolCursor) ||
        m_solutions[i].approximateLayout().isDownstreamOf(treePoolCursor)) {
      m_solutions[i] = Solution();
    }
  }
}

System::Error System::privateExactSolve(Context *context) {
  m_numberOfSolutions = 0;
  Expression simplifiedEquations[EquationStore::k_maxNumberOfEquations];
  Error error = simplifyAndFindVariables(context, simplifiedEquations);
  if (error != Error::NoError) {
    return error;
  }
  error = solveLinearSystem(context, simplifiedEquations);
  if (error != Error::NonLinearSystem || m_numberOfResolutionVariables > 1 ||
      m_store->numberOfDefinedModels() > 1) {
    return error;
  }
  error = solvePolynomial(context, simplifiedEquations);
  if (error == Error::RequireApproximateSolution) {
    m_type = Type::GeneralMonovariable;
  }
  assert(error != Error::NoError || m_type == Type::PolynomialMonovariable);
  return error;
}

System::Error System::simplifyAndFindVariables(
    Context *context, Expression *simplifiedEquations) {
  m_numberOfResolutionVariables = 0;
  m_numberOfUserVariables = 0;
  m_variables[0][0] = '\0';
  m_userVariables[0][0] = '\0';
  m_complexFormat = Preferences::sharedPreferences->complexFormat();

  EquationStore *store = m_store;
  size_t nEquations = store->numberOfDefinedModels();
  for (size_t i = 0; i < nEquations; i++) {
    ExpiringPointer<Equation> equation =
        store->modelForRecord(store->definedRecordAtIndex(i));
    Expression equationsWithUserVariables = equation->standardForm(
        context, true, ReductionTarget::SystemForAnalysis);

    // Gather user variables
    int nVariables = equationsWithUserVariables.getVariables(
        context,
        [](const char *s, Context *c) {
          return c->expressionTypeForIdentifier(s, strlen(s)) ==
                 Context::SymbolAbstractType::Symbol;
        },
        &m_userVariables[0][0], SymbolAbstractNode::k_maxNameSize,
        m_numberOfUserVariables);
    /* Don't abort if there are more the k_maxNumberOfVariables defined user
     * variables. */
    m_numberOfUserVariables =
        nVariables >= 0 ? nVariables : Expression::k_maxNumberOfVariables;

    simplifiedEquations[i] =
        m_overrideUserVariables
            ? equationsWithUserVariables
            : equation->standardForm(context, false,
                                     ReductionTarget::SystemForAnalysis);
    if (simplifiedEquations[i].isUninitialized() ||
        simplifiedEquations[i].type() == ExpressionNode::Type::Undefined ||
        simplifiedEquations[i].recursivelyMatches(
            Expression::IsMatrix, context,
            m_overrideUserVariables
                ? SymbolicComputation::ReplaceDefinedFunctionsWithDefinitions
                : SymbolicComputation::
                      ReplaceAllDefinedSymbolsWithDefinition)) {
      return Error::EquationUndefined;
    } else if (simplifiedEquations[i].type() == ExpressionNode::Type::Nonreal) {
      return Error::EquationNonreal;
    }

    m_complexFormat = Preferences::UpdatedComplexFormatWithExpressionInput(
        m_complexFormat, simplifiedEquations[i], nullptr);

    // Gather resolution variables
    m_numberOfResolutionVariables = simplifiedEquations[i].getVariables(
        context, [](const char *, Context *) { return true; },
        &m_variables[0][0], SymbolAbstractNode::k_maxNameSize,
        m_numberOfResolutionVariables);
    /* The equation has been parsed, so there should not be any variable with a
     * name that is too long. */
    // FIXME Special return values of getVariables should be named.
    assert(m_numberOfResolutionVariables != -2);
    if (m_numberOfResolutionVariables == -1) {
      return Error::TooManyVariables;
    }
  }
  return Error::NoError;
}

System::Error System::solveLinearSystem(Context *context,
                                        Expression *simplifiedEquations) {
  Preferences::AngleUnit angleUnit =
      Preferences::sharedPreferences->angleUnit();
  Preferences::UnitFormat unitFormat =
      GlobalPreferences::sharedGlobalPreferences->unitFormat();
  SymbolicComputation symbolicComputation =
      m_overrideUserVariables
          ? SymbolicComputation::ReplaceDefinedFunctionsWithDefinitions
          : SymbolicComputation::ReplaceAllDefinedSymbolsWithDefinition;
  Expression coefficients[EquationStore::k_maxNumberOfEquations]
                         [Expression::k_maxNumberOfVariables];
  Expression constants[EquationStore::k_maxNumberOfEquations];
  size_t m = m_store->numberOfDefinedModels();
  for (int i = 0; i < m; i++) {
    bool isLinear = simplifiedEquations[i].getLinearCoefficients(
        &m_variables[0][0], SymbolAbstractNode::k_maxNameSize, coefficients[i],
        &constants[i], context, m_complexFormat, angleUnit, unitFormat,
        symbolicComputation);
    if (!isLinear) {
      return Error::NonLinearSystem;
    }
  }
  m_degree = 1;
  m_type = Type::LinearSystem;

  m_hasMoreSolutions = false;
  // n unknown variables and m equations
  size_t n = m_numberOfResolutionVariables;
  // Create the matrix (A|b) for the equation Ax=b;
  Matrix ab = Matrix::Builder();
  size_t abChildren = 0;
  for (size_t i = 0; i < m; i++) {
    for (size_t j = 0; j < n; j++) {
      ab.addChildAtIndexInPlace(coefficients[i][j], abChildren, abChildren);
      ++abChildren;
    }
    ab.addChildAtIndexInPlace(constants[i], abChildren, abChildren);
    ++abChildren;
  }
  ab.setDimensions(m, n + 1);

  // Compute the rank of (A|b)
  int rank = ab.rank(context, m_complexFormat, angleUnit, unitFormat,
                     ReductionTarget::SystemForApproximation, true);
  if (rank == -1) {
    return Error::EquationUndefined;
  }

  for (int j = m - 1; j >= 0; j--) {
    if (ab.matrixChild(j, n).isNull(context) != TrinaryBoolean::True &&
        (j >= n ||
         ab.matrixChild(j, j).isNull(context) == TrinaryBoolean::True)) {
      /* Row j describes an equation of the form '0=b', the system has no
       * solution. */
      m_numberOfSolutions = 0;
      return Error::NoError;
    }
  }

  if (rank == n && n > 0) {
    /* The rank is equal to the number of variables: the system has n solutions,
     * and after canonization their values are the values on the last column. */
    assert(m_numberOfSolutions == 0);
    Error error;
    for (size_t i = 0; i < n; i++) {
      error =
          registerSolution(ab.matrixChild(i, n), context, SolutionType::Exact);
      if (error != Error::NoError) {
        break;
      }
    }
    assert(error != Error::NoError || m_numberOfSolutions == n);
    return error;
  }

  /* The system is insufficiently qualified: bind the value of n-rank
   * variables to parameters. */
  m_hasMoreSolutions = true;

  size_t parameterNameLength = n - rank == 1 ? 1 : 2;
  char parameterSuffix = '1';
  while (rank < n) {
    // Find the last unbound variable, i.e. the first row that is not
    // triangular, starting from the bottom.
    size_t variable;
    for (size_t k = 0; k < m; k++) {
      bool rowIsTriangular =
          ab.matrixChild(m - 1 - k, n - 1 - k).isNull(context) !=
          TrinaryBoolean::True;
      size_t x = 0;
      while (rowIsTriangular && x < n - 1 - k) {
        if (ab.matrixChild(m - 1 - k, x).isNull(context) !=
            TrinaryBoolean::True) {
          rowIsTriangular = false;
        }
        x++;
      }
      if (!rowIsTriangular) {
        variable = n - 1 - k;
        break;
      }
    }
    // Add the row variable=parameter to increase the rank of the system.
    for (size_t i = 0; i < n; i++) {
      ab.addChildAtIndexInPlace(Rational::Builder(i == variable ? 1 : 0),
                                abChildren, abChildren);
      ++abChildren;
    }
    assert(parameterSuffix <= '9');
    const char parameterName[] = {'t', static_cast<char>(parameterSuffix++),
                                  '\0'};
    ab.addChildAtIndexInPlace(
        Symbol::Builder(parameterName, parameterNameLength), abChildren,
        abChildren);
    ++abChildren;

    ab.setDimensions(++m, n + 1);
    rank = ab.rank(context, m_complexFormat, angleUnit, unitFormat,
                   ReductionTarget::SystemForAnalysis, true);
    if (rank == -1) {
      return Error::EquationUndefined;
    }
  }
  assert(rank == n);
  // System is fully qualified, register the parametric solutions.
  m_numberOfSolutions = 0;
  for (size_t i = 0; i < n; i++) {
    Error error =
        registerSolution(ab.matrixChild(i, n), context, SolutionType::Formal);
    if (error != Error::NoError) {
      return error;
    }
  }
  return Error::NoError;
}

System::Error System::solvePolynomial(Context *context,
                                      Expression *simplifiedEquations) {
  assert(m_numberOfResolutionVariables == 1 &&
         m_store->numberOfDefinedModels() == 1);
  Preferences::AngleUnit angleUnit =
      Preferences::sharedPreferences->angleUnit();
  Preferences::UnitFormat unitFormat =
      GlobalPreferences::sharedGlobalPreferences->unitFormat();
  SymbolicComputation symbolicComputation =
      m_overrideUserVariables
          ? SymbolicComputation::ReplaceDefinedFunctionsWithDefinitions
          : SymbolicComputation::ReplaceAllDefinedSymbolsWithDefinition;
  Expression coefficients[Expression::k_maxNumberOfPolynomialCoefficients];
  m_degree = simplifiedEquations[0].getPolynomialReducedCoefficients(
      m_variables[0], coefficients, context, m_complexFormat, angleUnit,
      unitFormat, symbolicComputation);
  if (m_degree < 2 || m_degree > 3) {
    return Error::RequireApproximateSolution;
  }

  m_type = Type::PolynomialMonovariable;
  ReductionContext reductionContext(context, m_complexFormat, angleUnit,
                                    unitFormat, ReductionTarget::User);
  Expression delta;
  Expression x[3];
  bool solutionsAreApproximate = false;
  size_t numberOfSolutions = 0;
  if (m_degree == 2) {
    numberOfSolutions = Poincare::Polynomial::QuadraticPolynomialRoots(
        coefficients[2], coefficients[1], coefficients[0], x, x + 1, &delta,
        reductionContext);
  } else {
    assert(m_degree == 3);
    numberOfSolutions = Poincare::Polynomial::CubicPolynomialRoots(
        coefficients[3], coefficients[2], coefficients[1], coefficients[0], x,
        x + 1, x + 2, &delta, reductionContext, &solutionsAreApproximate);
  }
  SolutionType type =
      solutionsAreApproximate ? SolutionType::Approximate : SolutionType::Exact;
  for (size_t i = 0; i < numberOfSolutions; i++) {
    Error error = registerSolution(x[i], context, type);
    if (error != Error::NoError) {
      return error;
    }
  }
  // Account for delta
  return registerSolution(delta, context, type);
}

System::Error System::registerSolution(Expression e, Context *context,
                                       SolutionType type) {
  Preferences::AngleUnit angleUnit =
      Preferences::sharedPreferences->angleUnit();
  Expression exact, approximate;

  if (type == SolutionType::Approximate) {
    approximate = e;
  } else {
    Preferences::UnitFormat unitFormat =
        GlobalPreferences::sharedGlobalPreferences->unitFormat();
    SymbolicComputation symbolicComputation =
        m_overrideUserVariables
            ? SymbolicComputation::ReplaceDefinedFunctionsWithDefinitions
            : SymbolicComputation::ReplaceAllDefinedSymbolsWithDefinition;
    e.cloneAndSimplifyAndApproximate(&exact, &approximate, context,
                                     m_complexFormat, angleUnit, unitFormat,
                                     symbolicComputation);
  }
  if (approximate.type() == ExpressionNode::Type::Nonreal) {
    return Error::NoError;
  }
  if (type != SolutionType::Formal &&
      approximate.type() == ExpressionNode::Type::Undefined) {
    return Error::EquationUndefined;
  }

  Layout exactLayout, approximateLayout;
  bool exactAndApproximateAreEqual = false;

  if (type != SolutionType::Approximate &&
      !ExpressionDisplayPermissions::ShouldNeverDisplayExactOutput(exact,
                                                                   context)) {
    exactLayout = PoincareHelpers::CreateLayout(exact, context);
  }
  if (type != SolutionType::Formal) {
    approximateLayout = PoincareHelpers::CreateLayout(approximate, context);
    if (type == SolutionType::Exact) {
      char exactBuffer[::Constant::MaxSerializedExpressionSize];
      char approximateBuffer[::Constant::MaxSerializedExpressionSize];
      exactLayout.serializeForParsing(exactBuffer,
                                      ::Constant::MaxSerializedExpressionSize);
      approximateLayout.serializeForParsing(
          approximateBuffer, ::Constant::MaxSerializedExpressionSize);
      if (strcmp(exactBuffer, approximateBuffer) == 0) {
        exactLayout = Layout();
      } else if (Expression::ExactAndApproximateExpressionsAreEqual(
                     exact, Expression::Parse(approximateBuffer, context))) {
        exactAndApproximateAreEqual = true;
      }
    }
  }

  assert(m_numberOfSolutions < k_maxNumberOfSolutions - 1);
  m_solutions[m_numberOfSolutions++] = Solution(
      exactLayout, approximateLayout, NAN, exactAndApproximateAreEqual);

  return Error::NoError;
}

void System::registerSolution(double f) {
  if (std::isfinite(f)) {
    m_solutions[m_numberOfSolutions++] = Solution(Layout(), Layout(), f, false);
  }
}

}  // namespace Solver