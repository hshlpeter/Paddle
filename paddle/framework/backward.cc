/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#include "paddle/framework/backward.h"

#include <list>
#include "paddle/framework/op_registry.h"
#include "paddle/operators/net_op.h"

namespace paddle {
namespace framework {

template <typename Map, typename T>
static void ForEachVarName(Map& names, T callback) {
  for (auto& name : names) {
    for (auto& n : name.second) {
      if (callback(n)) return;
    }
  }
}

static bool AllInSet(
    const std::map<std::string, std::vector<std::string>>& names,
    const std::string& suffix, const std::unordered_set<std::string>& set) {
  bool all_in_set = true;
  ForEachVarName(names, [&all_in_set, &set, &suffix](const std::string& n) {
    all_in_set = set.find(n + suffix) != set.end();
    return !all_in_set;
  });
  return all_in_set;
}

static std::shared_ptr<OperatorBase> NOP() {
  auto net_op = std::make_shared<operators::NetOp>();
  net_op->type_ = "@NOP@";
  net_op->CompleteAddOp();
  return net_op;
}

//  Get backward operator from a forward operator, recursively implementation.
//
//  no_grad_names the gradient variable names without gradient calculating.
//
//  uniq_id is a unique index used inside recursively calling
//  BackwardRecursive. use `uid = uniq_id++;` to get the unique index, and
//  pass `uniq_id` through recursive calling.
//
//  returns The backward operator. For simple situation, it is a simple
//  operator. For complex situation, it is a NetOp.
//
//  See Backward.h for details
static std::shared_ptr<OperatorBase> BackwardRecursive(
    const OperatorBase& forwardOp,
    std::unordered_set<std::string>& no_grad_names, size_t& uniq_id);
std::shared_ptr<OperatorBase> BackwardRecursive(
    const OperatorBase& forwardOp,
    std::unordered_set<std::string>& no_grad_names, size_t& uniq_id) {
  //  If all input gradients of forwarding operator do not need to calculate,
  //  just return an NOP. Not return null ptr because NOP does not take
  //  too much time for calculation, but it is useful for simplifying logic.
  if (AllInSet(forwardOp.inputs_, kGradVarSuffix, no_grad_names)) {
    return NOP();
  }

  //  All output gradients of forwarding operator do not need to calculate.
  //  Then all input gradients cannot be computed at all, and we put them into
  //  `no_grad_names` set. Return an NOP.
  if (AllInSet(forwardOp.outputs_, kGradVarSuffix, no_grad_names)) {
    ForEachVarName(forwardOp.inputs_,
                   [&no_grad_names](const std::string& name) -> bool {
                     no_grad_names.insert(GradVarName(name));
                     return false;
                   });
    return NOP();
  }

  // Returned gradient network
  auto net = std::make_shared<operators::NetOp>();

  if (forwardOp.IsNetOp()) {
    // Because forwardOp is a net op, it can static_cast.
    auto& forwardNet = static_cast<const operators::NetOp&>(forwardOp);

    // Map from output gradient variable name to operator's indices in
    // backward net. That operator generates that variable.
    std::unordered_map<std::string, std::vector<size_t>> dup_output_ops;

    size_t local_op_id = 0;
    // reversely travel forwardNet
    for (auto it = forwardNet.ops_.rbegin(); it != forwardNet.ops_.rend();
         ++it, ++local_op_id) {
      auto fwd = *it;
      auto bwd = BackwardRecursive(*fwd, no_grad_names, uniq_id);
      net->AddOp(bwd);
      ForEachVarName(bwd->outputs_,
                     [&dup_output_ops, local_op_id](const std::string& out) {
                       dup_output_ops[out].emplace_back(local_op_id);
                       return false;
                     });
    }
    // Get unique ID for this method.
    auto uid = uniq_id++;
    // TODO(dzh): more comment
    using Pos = std::pair<size_t, std::shared_ptr<OperatorBase>>;
    std::list<Pos> insert_position;
    for (auto& dup_output_op : dup_output_ops) {
      const std::string& name = dup_output_op.first;
      auto& dup_op = dup_output_op.second;
      if (dup_op.size() == 1) continue;
      std::vector<std::string> dup_outputs;

      for (size_t i = 0; i < dup_op.size(); ++i) {
        auto op_offset = dup_op[i];
        dup_outputs.push_back(name + "@RENAME@" + std::to_string(uid) + "@" +
                              std::to_string(i));
        net->ops_[op_offset]->Rename(name, dup_outputs.back());
      }
      insert_position.push_back(
          {dup_op.back(), OpRegistry::CreateOp("add", {{"X", {dup_outputs}}},
                                               {{"Out", {name}}}, {})});
    }

    insert_position.sort(
        [](const Pos& l, const Pos& r) { return l.first > r.first; });

    for (auto& pos : insert_position) {
      net->InsertOp(pos.first + 1, pos.second);
    }
  } else {
    std::shared_ptr<OperatorBase> grad_op = OpRegistry::CreateGradOp(forwardOp);

    ForEachVarName(grad_op->inputs_, [&no_grad_names,
                                      &net](std::string& grad_input) {
      if (no_grad_names.count(grad_input)) {
        // +1 for \0
        std::string prefix = grad_input.substr(
            0, grad_input.size() - sizeof(kGradVarSuffix) / sizeof(char) + 1);
        grad_input = prefix + kZeroVarSuffix;

        // If part of input gradient of that operator is not calculated, fill
        // zero variables to that input gradient.
        net->AddOp(OpRegistry::CreateOp("fill_zeros_like", {{"Src", {prefix}}},
                                        {{"Dst", {grad_input}}}, {}));
      }
      return false;
    });

    ForEachVarName(grad_op->outputs_,
                   [&no_grad_names](std::string& grad_output) {
                     if (no_grad_names.count(grad_output)) {
                       grad_output = kEmptyVarName;
                     }
                     return false;
                   });

    if (net->ops_.empty()) {  // Current no aux op is added to network
      return grad_op;
    }
    net->AddOp(grad_op);
  }
  net->type_ = "@GENERATED_BACKWARD@";
  net->CompleteAddOp();
  return net;
}  // namespace framework

// See header for comments
std::shared_ptr<OperatorBase> Backward(
    const OperatorBase& forwardOp,
    const std::unordered_set<std::string>& no_grad_vars) {
  std::unordered_set<std::string> no_grad_names;
  no_grad_names.reserve(no_grad_vars.size());

  no_grad_names.insert(std::string(kEmptyVarName) + kGradVarSuffix);

  for (auto& name : no_grad_vars) {
    no_grad_names.insert(name + kGradVarSuffix);
  }
  size_t uid = 0;
  return BackwardRecursive(forwardOp, no_grad_names, uid);
}

}  // namespace framework
}  // namespace paddle