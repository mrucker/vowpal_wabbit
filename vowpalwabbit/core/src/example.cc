// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.
#include "vw/core/example.h"

#include "vw/cache_parser/parse_example_cache.h"
#include "vw/core/cb_continuous_label.h"
#include "vw/core/interactions.h"
#include "vw/core/model_utils.h"
#include "vw/core/reductions/gd.h"
#include "vw/core/simple_label_parser.h"
#include "vw/core/text_utils.h"

#include <algorithm>
#include <climits>
#include <cstdint>

float calculate_total_sum_features_squared(bool permutations, VW::example& ec)
{
  float sum_features_squared = 0.f;
  for (const features& fs : ec) { sum_features_squared += fs.sum_feat_sq; }

  float calculated_sum_features_squared = INTERACTIONS::eval_sum_ft_squared_of_generated_ft(
      permutations, *ec.interactions, *ec.extent_interactions, ec.feature_space);
  sum_features_squared += calculated_sum_features_squared;
  return sum_features_squared;
}

VW::example::~example()
{
  if (passthrough)
  {
    delete passthrough;
    passthrough = nullptr;
  }
}

float VW::example::get_total_sum_feat_sq()
{
  if (!_total_sum_feat_sq_calculated)
  {
    total_sum_feat_sq = calculate_total_sum_features_squared(_use_permutations, *this);
    _total_sum_feat_sq_calculated = true;
  }
  return total_sum_feat_sq;
}

float collision_cleanup(features& fs)
{
  // This loops over the sequence of feature values and their indexes
  // when an index is repeated this combines them by adding their values.
  // This assumes that fs is sorted (which is the case in `flatten_sort_example`).

  float sum_sq = 0.f;
  if (!fs.empty())
  {
    features::iterator p1 = fs.begin();
    uint64_t last_index = p1.index();

    for (features::iterator p2 = (fs.begin() + 1); p2 != fs.end(); ++p2)
    {
      if (last_index == p2.index()) { p1.value() += p2.value(); }
      else
      {
        sum_sq += p1.value() * p1.value();
        ++p1;
        p1.value() = p2.value();
        p1.index() = p2.index();
        last_index = p2.index();
      }
    }

    sum_sq += p1.value() * p1.value();
    ++p1;

    fs.truncate_to(p1, 0);
    fs.sum_feat_sq = sum_sq;
  }

  return sum_sq;
}

namespace VW
{
void copy_example_label(example* dst, example* src, void (*)(polylabel*, polylabel*))
{
  dst->l = src->l;
  dst->ex_reduction_features = src->ex_reduction_features;
}

void copy_example_label(example* dst, const example* src) { dst->l = src->l; }

void copy_example_metadata(example* dst, const example* src)
{
  dst->tag = src->tag;
  dst->example_counter = src->example_counter;

  dst->ft_offset = src->ft_offset;

  dst->partial_prediction = src->partial_prediction;
  if (src->passthrough == nullptr) { dst->passthrough = nullptr; }
  else { dst->passthrough = new features(*src->passthrough); }
  dst->loss = src->loss;
  dst->weight = src->weight;
  dst->confidence = src->confidence;
  dst->test_only = src->test_only;
  dst->end_pass = src->end_pass;
  dst->is_newline = src->is_newline;
  dst->sorted = src->sorted;
}

void copy_example_data(example* dst, const example* src)
{
  copy_example_metadata(dst, src);

  // copy feature data
  dst->indices = src->indices;
  for (namespace_index c : src->indices) { dst->feature_space[c] = src->feature_space[c]; }
  dst->num_features = src->num_features;
  dst->total_sum_feat_sq = src->total_sum_feat_sq;
  dst->_total_sum_feat_sq_calculated = src->_total_sum_feat_sq_calculated;
  dst->_use_permutations = src->_use_permutations;
  dst->interactions = src->interactions;
  dst->extent_interactions = src->extent_interactions;
  dst->debug_current_reduction_depth = src->debug_current_reduction_depth;
}

void copy_example_data_with_label(example* dst, const example* src)
{
  copy_example_data(dst, src);
  copy_example_label(dst, src);
}

void move_feature_namespace(example* dst, example* src, namespace_index c)
{
  if (std::find(src->indices.begin(), src->indices.end(), c) == src->indices.end())
  {
    return;  // index not present in src
  }
  if (std::find(dst->indices.begin(), dst->indices.end(), c) == dst->indices.end()) { dst->indices.push_back(c); }

  auto& fdst = dst->feature_space[c];
  auto& fsrc = src->feature_space[c];

  src->num_features -= fsrc.size();
  src->reset_total_sum_feat_sq();
  std::swap(fdst, fsrc);
  dst->num_features += fdst.size();
  dst->reset_total_sum_feat_sq();
}

}  // namespace VW

class features_and_source
{
public:
  VW::v_array<feature> feature_map;  // map to store sparse feature vectors
  uint32_t stride_shift;
  uint64_t mask;
};

void vec_store(features_and_source& p, float fx, uint64_t fi)
{
  p.feature_map.push_back(feature(fx, (fi >> p.stride_shift) & p.mask));
}

namespace VW
{
feature* get_features(VW::workspace& all, example* ec, size_t& feature_map_len)
{
  features_and_source fs;
  fs.stride_shift = all.weights.stride_shift();
  fs.mask = all.weights.mask() >> all.weights.stride_shift();
  GD::foreach_feature<features_and_source, uint64_t, vec_store>(all, *ec, fs);

  auto* features_array = new feature[fs.feature_map.size()];
  std::memcpy(features_array, fs.feature_map.data(), fs.feature_map.size() * sizeof(feature));
  feature_map_len = fs.feature_map.size();
  return features_array;
}

void return_features(feature* f) { delete[] f; }
}  // namespace VW

class full_features_and_source
{
public:
  features fs;
  uint32_t stride_shift;
  uint64_t mask;
};

void vec_ffs_store(full_features_and_source& p, float fx, uint64_t fi)
{
  p.fs.push_back(fx, (fi >> p.stride_shift) & p.mask);
}
namespace VW
{
flat_example* flatten_example(VW::workspace& all, example* ec)
{
  flat_example& fec = calloc_or_throw<flat_example>();
  fec.l = ec->l;
  fec.tag = ec->tag;
  fec.ex_reduction_features = ec->ex_reduction_features;
  fec.example_counter = ec->example_counter;
  fec.ft_offset = ec->ft_offset;
  fec.num_features = ec->num_features;

  full_features_and_source ffs;
  ffs.stride_shift = all.weights.stride_shift();
  if (all.weights.not_null())
  {  // TODO:temporary fix. all.weights is not initialized at this point in some cases.
    ffs.mask = all.weights.mask() >> all.weights.stride_shift();
  }
  else { ffs.mask = static_cast<uint64_t>(LONG_MAX) >> all.weights.stride_shift(); }
  GD::foreach_feature<full_features_and_source, uint64_t, vec_ffs_store>(all, *ec, ffs);

  std::swap(fec.fs, ffs.fs);

  return &fec;
}

flat_example* flatten_sort_example(VW::workspace& all, example* ec)
{
  flat_example* fec = flatten_example(all, ec);
  fec->fs.sort(all.parse_mask);
  fec->total_sum_feat_sq = collision_cleanup(fec->fs);
  return fec;
}

void free_flatten_example(flat_example* fec)
{
  // note: The label memory should be freed by by freeing the original example.
  if (fec)
  {
    fec->fs.~features();
    free(fec);
  }
}

example* alloc_examples(size_t count) { return new VW::example[count]; }

void dealloc_examples(example* example_ptr, size_t /* count */) { delete[] example_ptr; }

void finish_example(VW::workspace&, example&);
void clean_example(VW::workspace&, example&);

void finish_example(VW::workspace& all, multi_ex& ec_seq)
{
  for (example* ecc : ec_seq) { VW::finish_example(all, *ecc); }
}

void return_multiple_example(VW::workspace& all, VW::multi_ex& examples)
{
  for (auto ec : examples) { clean_example(all, *ec); }
  examples.clear();
}
namespace details
{
void truncate_example_namespace(VW::example& ec, VW::namespace_index ns, const features& fs)
{
  // print_update is called after this del_example_namespace,
  // so we need to keep the ec.num_features correct,
  // so shared features are included in the reported number of "current features"
  // ec.num_features -= numf;
  features& del_target = ec.feature_space[static_cast<size_t>(ns)];
  assert(del_target.size() >= fs.size());
  assert(!ec.indices.empty());
  if (ec.indices.back() == ns && ec.feature_space[static_cast<size_t>(ns)].size() == fs.size())
  {
    ec.indices.pop_back();
  }
  ec.reset_total_sum_feat_sq();
  ec.num_features -= fs.size();
  del_target.truncate_to(del_target.size() - fs.size(), fs.sum_feat_sq);
}

void append_example_namespace(VW::example& ec, VW::namespace_index ns, const features& fs)
{
  const auto index_it = std::find(ec.indices.begin(), ec.indices.end(), ns);
  const bool has_ns = index_it != ec.indices.end();
  if (!has_ns) { ec.indices.push_back(ns); }

  features& add_fs = ec.feature_space[static_cast<size_t>(ns)];
  add_fs.concat(fs);
  ec.reset_total_sum_feat_sq();
  ec.num_features += fs.size();
}

void append_example_namespaces_from_example(VW::example& target, const VW::example& source)
{
  for (VW::namespace_index idx : source.indices)
  {
    if (idx == VW::details::CONSTANT_NAMESPACE) { continue; }
    append_example_namespace(target, idx, source.feature_space[idx]);
  }
}

void truncate_example_namespaces_from_example(VW::example& target, const VW::example& source)
{
  if (source.indices.empty())
  {  // making sure we can deal with empty shared example
    return;
  }
  auto idx = source.indices.end();
  idx--;
  for (; idx >= source.indices.begin(); idx--)
  {
    if (*idx == VW::details::CONSTANT_NAMESPACE) { continue; }
    truncate_example_namespace(target, *idx, source.feature_space[*idx]);
  }
}
}  // namespace details

namespace model_utils
{
size_t read_model_field(io_buf& io, flat_example& fe, VW::label_parser& lbl_parser)
{
  size_t bytes = 0;
  lbl_parser.default_label(fe.l);
  bytes += lbl_parser.read_cached_label(fe.l, fe.ex_reduction_features, io);
  bytes += read_model_field(io, fe.tag);
  bytes += read_model_field(io, fe.example_counter);
  bytes += read_model_field(io, fe.ft_offset);
  bytes += read_model_field(io, fe.global_weight);
  bytes += read_model_field(io, fe.num_features);
  bytes += read_model_field(io, fe.total_sum_feat_sq);
  unsigned char index = 0;
  bytes += ::VW::parsers::cache::details::read_cached_index(io, index);
  bool sorted = true;
  bytes += ::VW::parsers::cache::details::read_cached_features(io, fe.fs, sorted);
  return bytes;
}
size_t write_model_field(io_buf& io, const flat_example& fe, const std::string& upstream_name, bool text,
    VW::label_parser& lbl_parser, uint64_t parse_mask)
{
  size_t bytes = 0;
  lbl_parser.cache_label(fe.l, fe.ex_reduction_features, io, upstream_name + "_label", text);
  bytes += write_model_field(io, fe.tag, upstream_name + "_tag", text);
  bytes += write_model_field(io, fe.example_counter, upstream_name + "_example_counter", text);
  bytes += write_model_field(io, fe.ft_offset, upstream_name + "_ft_offset", text);
  bytes += write_model_field(io, fe.global_weight, upstream_name + "_global_weight", text);
  bytes += write_model_field(io, fe.num_features, upstream_name + "_num_features", text);
  bytes += write_model_field(io, fe.total_sum_feat_sq, upstream_name + "_total_sum_feat_sq", text);
  ::VW::parsers::cache::details::cache_index(io, 0);
  ::VW::parsers::cache::details::cache_features(io, fe.fs, parse_mask);
  return bytes;
}
}  // namespace model_utils
}  // namespace VW

namespace VW
{
std::string to_string(const v_array<float>& scalars, int decimal_precision)
{
  std::stringstream ss;
  std::string delim;
  for (float f : scalars)
  {
    ss << delim << VW::fmt_float(f, decimal_precision);
    delim = ",";
  }
  return ss.str();
}
}  // namespace VW
