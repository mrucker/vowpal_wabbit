// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#include "vw/core/reductions/eigen_memory_tree.h"

#include "vw/core/example.h"
#include "vw/core/learner.h"
#include "vw/core/vw.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace VW::reductions::eigen_memory_tree;

namespace eigen_memory_tree_test
{
emt_tree* get_emt_tree(VW::workspace& all)
{
  std::vector<std::string> e_r;
  all.l->get_enabled_reductions(e_r);
  if (std::find(e_r.begin(), e_r.end(), "eigen_memory_tree") == e_r.end())
  {
    ADD_FAILURE() << "Eigen memory tree not found in enabled reductions";
  }

  VW::LEARNER::single_learner* emt = as_singleline(all.l->get_learner_by_name_prefix("eigen_memory_tree"));

  return (emt_tree*)emt->get_internal_type_erased_data_pointer_test_use_only();
}

TEST(emt_tests, emt_params_test)
{
  auto* vw = VW::initialize("--eigen_memory_tree --quiet");
  auto* tree = get_emt_tree(*vw);

  EXPECT_EQ(tree->tree_bound, 0);
  EXPECT_EQ(tree->leaf_split, 100);
  EXPECT_EQ(tree->scorer_type, emt_scorer_type::self_consistent_rank);
  EXPECT_EQ(tree->router_type, emt_router_type::eigen);

  VW::finish(*vw);

  vw = VW::initialize("--eigen_memory_tree --tree 20 --scorer 2 --router 1 --leaf 50 --quiet");
  tree = get_emt_tree(*vw);

  EXPECT_EQ(tree->tree_bound, 20);
  EXPECT_EQ(tree->leaf_split, 50);
  EXPECT_EQ(tree->scorer_type, emt_scorer_type::distance);
  EXPECT_EQ(tree->router_type, emt_router_type::random);

  VW::finish(*vw);
}

TEST(emt_tests, emt_exact_match_sans_router_test)
{
  auto& vw = *VW::initialize("--eigen_memory_tree --quiet");

  VW::example& ex1 = *VW::read_example(vw, "1 | 1 2 3");
  VW::example& ex2 = *VW::read_example(vw, "2 | 2 3 4");

  vw.learn(ex1);
  vw.learn(ex2);

  EXPECT_EQ(ex1.pred.multiclass, 0);
  EXPECT_EQ(ex2.pred.multiclass, 1);

  vw.predict(ex1);
  vw.predict(ex2);

  EXPECT_EQ(ex1.pred.multiclass, 1);
  EXPECT_EQ(ex2.pred.multiclass, 2);

  vw.finish_example(ex1);
  vw.finish_example(ex2);
  VW::finish(vw);
}

TEST(emt_tests, emt_exact_match_with_router_test)
{
  auto& vw = *VW::initialize("--eigen_memory_tree --quiet --leaf 5");

  std::vector<VW::example*> examples;

  for (int i = 0; i < 10; i++)
  {
    examples.push_back(VW::read_example(vw, std::to_string(i) + " | " + std::to_string(i)));
    vw.learn(*examples[i]);
  }

  for (int i = 0; i < 10; i++)
  {
    vw.predict(*examples[i]);
    EXPECT_EQ(examples[i]->pred.multiclass, i);
    vw.finish_example(*examples[i]);
  }

  VW::finish(vw);
}

TEST(emt_tests, emt_bounding)
{
  auto& vw = *VW::initialize("--eigen_memory_tree --quiet --tree 5");
  auto& tree = *get_emt_tree(vw);

  std::vector<VW::example*> examples;

  for (int i = 0; i < 10; i++)
  {
    examples.push_back(VW::read_example(vw, std::to_string(i) + " | " + std::to_string(i)));
    vw.learn(*examples[i]);
  }

  EXPECT_EQ(tree.bounder->list.size(), 5);
  EXPECT_EQ(tree.root->examples.size(), 5);
  EXPECT_EQ(tree.root->router_weights.size(), 0);

  for (int i = 0; i < 10; i++) { vw.finish_example(*examples[i]); }
  VW::finish(vw);
}

TEST(emt_tests, emt_split)
{
  auto& vw = *VW::initialize("--eigen_memory_tree --quiet --leaf 3 --tree 10");
  auto& tree = *get_emt_tree(vw);

  std::vector<VW::example*> examples;

  for (int i = 0; i < 4; i++)
  {
    examples.push_back(VW::read_example(vw, std::to_string(i) + " | " + std::to_string(i)));
    vw.learn(*examples[i]);
  }

  EXPECT_EQ(tree.bounder->list.size(), 4);

  EXPECT_EQ(tree.root->examples.size(), 0);
  EXPECT_EQ(tree.root->left->examples.size(), 2);
  EXPECT_EQ(tree.root->right->examples.size(), 2);

  EXPECT_GE(tree.root->router_weights.size(), 0);
  EXPECT_EQ(tree.root->right->router_weights.size(), 0);
  EXPECT_EQ(tree.root->left->router_weights.size(), 0);

  for (int i = 0; i < 4; i++) { vw.finish_example(*examples[i]); }
  VW::finish(vw);
}
}  // namespace eigen_memory_tree_test
