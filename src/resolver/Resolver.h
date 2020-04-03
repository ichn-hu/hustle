#ifndef HUSTLE_RESOLVER_H
#define HUSTLE_RESOLVER_H

#include <vector>
#include <cassert>
#include <api/HustleDB.h>

#include "catalog/TableSchema.h"
#include "parser/ParseTree.h"
#include "resolver/Plan.h"

namespace hustle {
namespace resolver {

class Resolver {
 public:

  Resolver(hustle::catalog::Catalog *catalog) : catalog_(catalog) {}

  /**
   * This function takes as input a parse tree from the parser, builds the
   * plan from bottom to top given the predicates and expressions preserved
   * in the parse tree.
   *
   * First of all, a number of TableReference objects are built. On top of
   * each TableReference, a Select object is built to contain it. By
   * traversing the parse_tree->other_pred, we match each select predicate as
   * a filter onto the corresponding Select object. This step is to push all
   * the select predicates down to the TableReference objects as much as
   * possible. The Join objects are then built on top of these Select objects
   * by traversing parse_tree->loop_pred. The topmost Join object is then
   * captured as an input to the Aggregate, Project, and OrderBy.
   *
   * The resolving order is: TableReference, Select, Join, Aggregate, Project
   * and OrderBy.
   *
   * @param parse_tree: input parse tree from parser
   * @param hustleDB: input hustle database
   */
  void resolve(const std::shared_ptr<hustle::parser::ParseTree> &parse_tree) {
    // initialize the maps given parse_tree->tableList
    for (int i = 0; i < parse_tree->tableList.size(); i++) {
      std::string name = parse_tree->tableList[i];
      map_vir_to_real[i] = catalog_->getTableIdbyName(name);
      map_real_to_vir[catalog_->getTableIdbyName(name)] = i;
    }

    // resolve TableReference
    std::vector<std::shared_ptr<Select>>
        select_operators(parse_tree->tableList.size());
    for (int i = 0; i < select_operators.size(); i++) {
      select_operators[i] = std::make_shared<Select>(
          std::make_shared<TableReference>(map_vir_to_real[i],
                                           parse_tree->tableList[i]));
    }

    // resolve Select
    for (auto &pred : parse_tree->other_pred) {
      if (pred->type == +ExprType::Comparative) {
        auto e = resolveComparative(
            std::dynamic_pointer_cast<hustle::parser::Comparative>(pred));
        select_operators[map_real_to_vir[e->left->i_table]]->filter.push_back(e);
      } else { // pred->type == +ExprType::Disjunctive
        assert(pred->type == +ExprType::Disjunctive);
        auto e = resolveDisjunctive(
            std::dynamic_pointer_cast<hustle::parser::Disjunctive>(pred));
        select_operators[map_real_to_vir[e->i_table]]->filter.push_back(e);
      }
    }

    // resolve Join
    std::shared_ptr<QueryOperator> root;
    for (auto &lpred : parse_tree->loop_pred) {
      if (root == nullptr) {
        root = std::move(select_operators[lpred->fromtable]);
      } else {
        std::vector<std::shared_ptr<Comparative>> join_preds;
        for (auto &pred : lpred->predicates) {
          join_preds.push_back(resolveComparative(pred));
        }
        auto right = std::move(select_operators[lpred->fromtable]);
        root = std::make_shared<Join>(
            std::move(root),
            std::move(right),
            std::move(join_preds)
        );
      }
    }

    // resolve Aggregate
    if (!parse_tree->aggregate.empty()) {
      // there can be at most one aggregate function
      auto aggregate_func = resolveAggFunc(parse_tree->aggregate[0]);

      std::vector<std::shared_ptr<ColumnReference>> groupby_cols;
      for (auto &col : parse_tree->group_by) {
        groupby_cols.push_back(resolveColumnReference(col));
      }
      root = std::make_shared<Aggregate>(
          std::move(root),
          std::move(aggregate_func),
          std::move(groupby_cols));
    } else {
      // if no aggragate, there must be no groupby
      assert(parse_tree->group_by.empty());
    }

    // resolve Project
    {
      std::vector<std::shared_ptr<Expr>> proj_exprs;
      std::vector<std::string> proj_names;

      for (auto &proj : parse_tree->project) {
        proj_exprs.push_back(resolveExpr(proj->expr));
        proj_names.push_back(proj->proj_name);
      }
      root = std::make_shared<Project>(std::move(root),
                                       std::move(proj_exprs),
                                       std::move(proj_names));
    }

    // resolve OrderBy
    if (!parse_tree->order_by.empty()) {
      std::vector<std::shared_ptr<ColumnReference>> orderby_cols;
      std::vector<OrderByDirection> orders;

      for (auto &orderby : parse_tree->order_by) {
        orderby_cols.push_back(resolveColumnReference(orderby->orderby_col));
        orders.push_back(orderby->order);
      }
      root = std::make_shared<OrderBy>(std::move(root),
                                       std::move(orderby_cols),
                                       std::move(orders));
    }

    plan_ = std::make_shared<Query>(root);
  }

  /**
   * Function to resolve hustle::parser::Expr
   */
  std::shared_ptr<Expr> resolveExpr(
      const std::shared_ptr<hustle::parser::Expr> &expr) {
    switch (expr->type) {
      case ExprType::ColumnReference:
        return resolveColumnReference(
            std::dynamic_pointer_cast<hustle::parser::ColumnReference>(expr));

      case ExprType::IntLiteral:
        return resolveIntLiteral(
            std::dynamic_pointer_cast<hustle::parser::IntLiteral>(expr));

      case ExprType::StrLiteral:
        return resolveStrLiteral(
            std::dynamic_pointer_cast<hustle::parser::StrLiteral>(expr));

      case ExprType::Comparative:
        return resolveComparative(
            std::dynamic_pointer_cast<hustle::parser::Comparative>(expr));

      case ExprType::Disjunctive:
        return resolveDisjunctive(
            std::dynamic_pointer_cast<hustle::parser::Disjunctive>(expr));

      case ExprType::Arithmetic:
        return resolveArithmetic(
            std::dynamic_pointer_cast<hustle::parser::Arithmetic>(expr));

      case ExprType::AggFunc:
        return resolveAggFunc(
            std::dynamic_pointer_cast<hustle::parser::AggFunc>(expr));

      default:
        return nullptr;
    }
  }

  /**
   * Function to resolve hustle::parser::Comparative
   */
  std::shared_ptr<Comparative> resolveComparative(
      const std::shared_ptr<hustle::parser::Comparative> &expr) {
    return std::make_shared<Comparative>(resolveColumnReference(
        std::dynamic_pointer_cast<hustle::parser::ColumnReference>(expr->left)),
                                         expr->op,
                                         resolveExpr(expr->right));
  }

  /**
   * Function to resolve hustle::parser::ColumnReference
   */
  std::shared_ptr<ColumnReference> resolveColumnReference(
      const std::shared_ptr<hustle::parser::ColumnReference> &expr) {
    return std::make_shared<ColumnReference>(expr->column_name,
                                             map_vir_to_real[expr->i_table],
                                             expr->i_column);
  }

  /**
   * Function to resolve hustle::parser::IntLiteral
   */
  std::shared_ptr<IntLiteral> resolveIntLiteral(
      const std::shared_ptr<hustle::parser::IntLiteral> &expr) {
    return std::make_shared<IntLiteral>(expr->value);
  }

  /**
   * Function to resolve hustle::parser::StrLiteral
   */
  std::shared_ptr<StrLiteral> resolveStrLiteral(
      const std::shared_ptr<hustle::parser::StrLiteral> &expr) {
    return std::make_shared<StrLiteral>(expr->value);
  }

  /**
   * Function to resolve hustle::parser::Disjunctive
   */
  std::shared_ptr<Disjunctive> resolveDisjunctive(
      const std::shared_ptr<hustle::parser::Disjunctive> &expr) {
    auto left = resolveComparative(
        std::dynamic_pointer_cast<hustle::parser::Comparative>(expr->left));
    auto right = resolveComparative(
        std::dynamic_pointer_cast<hustle::parser::Comparative>(expr->right));
    int i_table = left->left->i_table;

    return std::make_shared<Disjunctive>(
        i_table,
        std::vector<std::shared_ptr<Comparative>>{
            std::move(left),
            std::move(right)
        });
  }

  /**
   * Function to resolve hustle::parser::Arithmetic
   */
  std::shared_ptr<Arithmetic> resolveArithmetic(
      const std::shared_ptr<hustle::parser::Arithmetic> &expr) {
    return std::make_shared<Arithmetic>(resolveExpr(expr->left),
                                        expr->op,
                                        resolveExpr(expr->right));
  }

  /**
   * Function to resolve hustle::parser::AggFunc
   */
  std::shared_ptr<AggFunc> resolveAggFunc(
      const std::shared_ptr<hustle::parser::AggFunc> &expr) {
    return std::make_shared<AggFunc>(expr->func,
                                     resolveExpr(expr->expr));
  }

  /**
   * Function to return the plan
   * @return the plan
   */
  std::shared_ptr<Plan> getPlan() {
    return plan_;
  }

  /**
   * Function to serialize the parse tree
   */
  std::string toString(int indent) {
    json j = plan_;
    return j.dump(indent);
  }

 private:
  hustle::catalog::Catalog *catalog_;
  std::shared_ptr<Plan> plan_;
  std::map<int, int> map_vir_to_real; // matching from virtual to real table id
  std::map<int, int> map_real_to_vir; // matching from real to virtual table id
};

}
}

#endif //HUSTLE_RESOLVER_H
