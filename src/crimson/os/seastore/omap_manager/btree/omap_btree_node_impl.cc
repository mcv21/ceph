// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <string.h>
#include "include/buffer.h"
#include "include/byteorder.h"
#include "crimson/os/seastore/transaction_manager.h"
#include "crimson/os/seastore/omap_manager/btree/omap_btree_node.h"
#include "crimson/os/seastore/omap_manager/btree/omap_btree_node_impl.h"
#include "seastar/core/thread.hh"

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_filestore);
  }
}

namespace crimson::os::seastore::omap_manager {

std::ostream &operator<<(std::ostream &out, const omap_inner_key_t &rhs)
{
  return out << "omap_inner_key (" << rhs.key_off<< " - " << rhs.key_len
             << " - " << rhs.laddr << ")";
}

std::ostream &operator<<(std::ostream &out, const omap_leaf_key_t &rhs)
{
  return out << "omap_leaf_key_t (" << rhs.key_off<< " - " << rhs.key_len
             << " "<< rhs.val_off<<" - " << rhs.val_len << ")";
}

std::ostream &OMapInnerNode::print_detail_l(std::ostream &out) const
{
  return out << ", size=" << get_size()
	     << ", depth=" << get_meta().depth;
}

using dec_ref_ertr = OMapInnerNode::base_ertr;
using dec_ref_ret = dec_ref_ertr::future<>;
template <typename T>
dec_ref_ret dec_ref(omap_context_t oc, T&& addr) {
  return oc.tm.dec_ref(oc.t, std::forward<T>(addr)).handle_error(
    dec_ref_ertr::pass_further{},
    crimson::ct_error::assert_all{
      "Invalid error in OMapInnerNode helper dec_ref"
    }
  ).safe_then([](auto &&e) {});
}

/**
 * make_split_insert
 *
 * insert an  entry at iter, with the address of key.
 * will result in a split outcome encoded in the returned mutation_result_t
 */
OMapInnerNode::make_split_insert_ret
OMapInnerNode::make_split_insert(omap_context_t oc, internal_iterator_t iter,
                                 std::string key, laddr_t laddr)
{
  return make_split_children(oc).safe_then([=] (auto tuple) {
    auto [left, right, pivot] = tuple;
    if (pivot > key) {
      auto liter = left->iter_idx(iter.get_index());
      left->journal_inner_insert(liter, laddr, key,
                                 left->maybe_get_delta_buffer());
    } else {  //right
      auto riter = right->iter_idx(iter.get_index() - left->get_node_size());
      right->journal_inner_insert(riter, laddr, key,
                                  right->maybe_get_delta_buffer());
    }
    return make_split_insert_ret(
           make_split_insert_ertr::ready_future_marker{},
           mutation_result_t(mutation_status_t::WAS_SPLIT, tuple, std::nullopt));
  });

}


OMapInnerNode::handle_split_ret
OMapInnerNode::handle_split(omap_context_t oc, internal_iterator_t iter,
                               mutation_result_t mresult)
{
  logger().debug("OMapInnerNode: {}",  __func__);
  if (!is_pending()) {
    auto mut = oc.tm.get_mutable_extent(oc.t, this)->cast<OMapInnerNode>();
    auto mut_iter = mut->iter_idx(iter.get_index());
    return mut->handle_split(oc, mut_iter, mresult);
  }
  auto [left, right, pivot] = *(mresult.split_tuple);
  //update operation will not cause node overflow, so we can do it first.
  journal_inner_update(iter, left->get_laddr(), maybe_get_delta_buffer());
  bool overflow = extent_will_overflow(pivot.size() + 1, std::nullopt);
  if (!overflow) {
    journal_inner_insert(iter + 1, right->get_laddr(), pivot,
                         maybe_get_delta_buffer());
    return insert_ret(
           insert_ertr::ready_future_marker{},
           mutation_result_t(mutation_status_t::SUCCESS, std::nullopt, std::nullopt));
  } else {
    return make_split_insert(oc, iter + 1, pivot, right->get_laddr())
      .safe_then([this, oc] (auto m_result) {
       return dec_ref(oc, get_laddr())
         .safe_then([m_result = std::move(m_result)] {
          return insert_ret(
                 insert_ertr::ready_future_marker{},
                 m_result);
       });
   });
  }
}

OMapInnerNode::get_value_ret
OMapInnerNode::get_value(omap_context_t oc, const std::string &key)
{
  logger().debug("OMapInnerNode: {} key = {}",  __func__, key);
  auto child_pt = get_containing_child(key);
  auto laddr = child_pt->get_node_key().laddr;
  return omap_load_extent(oc, laddr, get_meta().depth - 1).safe_then(
    [oc, &key] (auto extent) {
    return extent->get_value(oc, key);
  }).finally([ref = OMapNodeRef(this)] {});
}

OMapInnerNode::insert_ret
OMapInnerNode::insert(omap_context_t oc, const std::string &key, const std::string &value)
{
  logger().debug("OMapInnerNode: {}  {}->{}",  __func__, key, value);
  auto child_pt = get_containing_child(key);
  assert(child_pt != iter_end());
  auto laddr = child_pt->get_node_key().laddr;
  return omap_load_extent(oc, laddr, get_meta().depth - 1).safe_then(
    [this, oc, child_pt, &key, &value] (auto extent) {
    return extent->insert(oc, key, value);
  }).safe_then([this, oc, child_pt] (auto mresult) {
    if (mresult.status == mutation_status_t::SUCCESS) {
      return insert_ertr::make_ready_future<mutation_result_t>(mresult);
    } else if (mresult.status == mutation_status_t::WAS_SPLIT) {
      return handle_split(oc, child_pt, mresult);
    } else {
     return insert_ret(
            insert_ertr::ready_future_marker{},
            mutation_result_t(mutation_status_t::SUCCESS, std::nullopt, std::nullopt));
    }
  });
}

OMapInnerNode::rm_key_ret
OMapInnerNode::rm_key(omap_context_t oc, const std::string &key)
{
  logger().debug("OMapInnerNode: {}", __func__);
  auto child_pt = get_containing_child(key);
  auto laddr = child_pt->get_node_key().laddr;
  return omap_load_extent(oc, laddr, get_meta().depth - 1).safe_then(
    [this, oc, &key, child_pt] (auto extent) {
    return extent->rm_key(oc, key)
      .safe_then([this, oc, child_pt, extent = std::move(extent)] (auto mresult) {
      switch (mresult.status) {
        case mutation_status_t::SUCCESS:
        case mutation_status_t::FAIL:
          return rm_key_ertr::make_ready_future<mutation_result_t>(mresult);
        case mutation_status_t::NEED_MERGE: {
          if (get_node_size() >1)
            return merge_entry(oc, child_pt, *(mresult.need_merge));
          else
            return rm_key_ret(
                   rm_key_ertr::ready_future_marker{},
                   mutation_result_t(mutation_status_t::SUCCESS,
                                     std::nullopt, std::nullopt));
        }
        case mutation_status_t::WAS_SPLIT:
          return handle_split(oc, child_pt, mresult);
        default:
          return rm_key_ertr::make_ready_future<mutation_result_t>(mresult);
      }
    });
  });
}

OMapInnerNode::list_keys_ret
OMapInnerNode::list_keys(omap_context_t oc, std::string &start, size_t max_result_size)
{
  logger().debug("OMapInnerNode: {}", __func__);
  auto  child_iter = get_containing_child(start);

  return seastar::do_with(child_iter, iter_end(), list_keys_result_t(), [=, &start]
    (auto &biter, auto &eiter, auto &result) {
    result.next = start;
    return crimson::do_until([=, &biter, &eiter, &result] ()
       -> list_keys_ertr::future<bool> {
      if (biter == eiter  || result.keys.size() == max_result_size)
        return list_keys_ertr::make_ready_future<bool>(true);

      auto laddr = biter->get_node_key().laddr;
      return omap_load_extent(oc, laddr, get_meta().depth - 1).safe_then(
        [=, &biter, &eiter, &result] (auto &&extent) {
        assert(result.next != std::nullopt);
        return extent->list_keys(oc, result.next.value(), max_result_size - result.keys.size())
          .safe_then([&biter, &eiter, &result] (auto &&child_result) mutable {
          result.keys.insert(result.keys.end(), child_result.keys.begin(),
                             child_result.keys.end());
          biter++;
          if (child_result.next == std::nullopt && biter != eiter)
            result.next = biter->get_node_val();
          else
            result.next = child_result.next;

          return list_keys_ertr::make_ready_future<bool>(false);
        });
      });
    }).safe_then([&result, ref = OMapNodeRef(this)] {
      return list_keys_ertr::make_ready_future<list_keys_result_t>(std::move(result));
    });
  });
}

OMapInnerNode::list_ret
OMapInnerNode::list(omap_context_t oc, std::string &start, size_t max_result_size)
{
  logger().debug("OMapInnerNode: {}", __func__);
  auto child_iter = get_containing_child(start);

  return seastar::do_with(child_iter, iter_end(), list_kvs_result_t(), [=, &start]
    (auto &biter, auto &eiter, auto &result) {
    result.next = start;
    return crimson::do_until([=, &biter, &eiter, &result] ()
      -> list_ertr::future<bool> {
      if (biter == eiter  || result.kvs.size() == max_result_size)
        return list_ertr::make_ready_future<bool>(true);

      auto laddr = biter->get_node_key().laddr;
      return omap_load_extent(oc, laddr, get_meta().depth - 1).safe_then(
        [=, &biter, &eiter, &result] (auto &&extent) {
        assert(result.next != std::nullopt);
        return extent->list(oc, result.next.value(), max_result_size - result.kvs.size())
          .safe_then([&biter, &eiter, &result] (auto &&child_result) mutable {
          result.kvs.insert(result.kvs.end(), child_result.kvs.begin(),
                              child_result.kvs.end());
          biter++;
          if (child_result.next == std::nullopt && biter != eiter)
            result.next = biter->get_node_val();
          else
            result.next = child_result.next;

          return list_ertr::make_ready_future<bool>(false);
        });
      });
    }).safe_then([&result, ref = OMapNodeRef(this)] {
      return list_ertr::make_ready_future<list_kvs_result_t>(std::move(result));
    });
  });
}

OMapInnerNode::clear_ret
OMapInnerNode::clear(omap_context_t oc)
{
  logger().debug("OMapInnerNode: {}", __func__);
  return crimson::do_for_each(iter_begin(), iter_end(), [this, oc] (auto iter) {
    auto laddr = iter->get_node_key().laddr;
    return omap_load_extent(oc, laddr, get_meta().depth - 1).safe_then(
      [oc] (auto &&extent) {
      return extent->clear(oc);
    }).safe_then([oc, laddr] {
      return dec_ref(oc, laddr);
    }).safe_then([ref = OMapNodeRef(this)] {
      return clear_ertr::now();
    });
  });
}

OMapInnerNode::split_children_ret
OMapInnerNode:: make_split_children(omap_context_t oc)
{
  logger().debug("OMapInnerNode: {}", __func__);
  return oc.tm.alloc_extents<OMapInnerNode>(oc.t, L_ADDR_MIN, OMAP_BLOCK_SIZE, 2)
    .safe_then([this] (auto &&ext_pair) {
      auto left = ext_pair.front();
      auto right = ext_pair.back();
      return split_children_ret(
             split_children_ertr::ready_future_marker{},
             std::make_tuple(left, right, split_into(*left, *right)));
  });
}

OMapInnerNode::full_merge_ret
OMapInnerNode::make_full_merge(omap_context_t oc, OMapNodeRef right)
{
  logger().debug("OMapInnerNode: {}", __func__);
  return oc.tm.alloc_extent<OMapInnerNode>(oc.t, L_ADDR_MIN, OMAP_BLOCK_SIZE)
    .safe_then([this, right] (auto &&replacement) {
      replacement->merge_from(*this, *right->cast<OMapInnerNode>());
      return full_merge_ret(
        full_merge_ertr::ready_future_marker{},
        std::move(replacement));
  });
}

OMapInnerNode::make_balanced_ret
OMapInnerNode::make_balanced(omap_context_t oc, OMapNodeRef _right)
{
  logger().debug("OMapInnerNode: {}", __func__);
  ceph_assert(_right->get_type() == type);
  return oc.tm.alloc_extents<OMapInnerNode>(oc.t, L_ADDR_MIN, OMAP_BLOCK_SIZE, 2)
    .safe_then([this, _right] (auto &&replacement_pair){
      auto replacement_left = replacement_pair.front();
      auto replacement_right = replacement_pair.back();
      auto &right = *_right->cast<OMapInnerNode>();
      return make_balanced_ret(
             make_balanced_ertr::ready_future_marker{},
             std::make_tuple(replacement_left, replacement_right,
                             balance_into_new_nodes(*this, right,
                               *replacement_left, *replacement_right)));
  });
}

OMapInnerNode::merge_entry_ret
OMapInnerNode::merge_entry(omap_context_t oc, internal_iterator_t iter, OMapNodeRef entry)
{
  logger().debug("OMapInnerNode: {}", __func__);
  if (!is_pending()) {
    auto mut = oc.tm.get_mutable_extent(oc.t, this)->cast<OMapInnerNode>();
    auto mut_iter = mut->iter_idx(iter->get_index());
    return mut->merge_entry(oc, mut_iter, entry);
  }
  auto is_left = (iter + 1) == iter_end();
  auto donor_iter = is_left ? iter - 1 : iter + 1;
  return omap_load_extent(oc, donor_iter->get_node_key().laddr,  get_meta().depth - 1)
    .safe_then([=] (auto &&donor) mutable {
    auto [l, r] = is_left ?
      std::make_pair(donor, entry) : std::make_pair(entry, donor);
    auto [liter, riter] = is_left ?
      std::make_pair(donor_iter, iter) : std::make_pair(iter, donor_iter);
    if (donor->extent_is_below_min()) {
      logger().debug("{}::merge_entry make_full_merge l {} r {}", __func__, *l, *r);
      assert(entry->extent_is_below_min());
      return l->make_full_merge(oc, r).safe_then([=] (auto &&replacement){
        journal_inner_update(liter, replacement->get_laddr(), maybe_get_delta_buffer());
        journal_inner_remove(riter, maybe_get_delta_buffer());
        //retire extent
        std::vector<laddr_t> dec_laddrs {l->get_laddr(), r->get_laddr()};
        return dec_ref(oc, dec_laddrs).safe_then([this, oc] {
          if (extent_is_below_min()) {
            return merge_entry_ret(
                   merge_entry_ertr::ready_future_marker{},
                   mutation_result_t(mutation_status_t::NEED_MERGE, std::nullopt,
                                    this));
          } else {
            return merge_entry_ret(
                   merge_entry_ertr::ready_future_marker{},
                   mutation_result_t(mutation_status_t::SUCCESS, std::nullopt, std::nullopt));
          }
        });
      });
    } else {
      logger().debug("{}::merge_entry balanced l {} r {}", __func__, *l, *r);
      return l->make_balanced(oc, r).safe_then([=] (auto tuple) {
        auto [replacement_l, replacement_r, replacement_pivot] = tuple;
        //update operation will not cuase node overflow, so we can do it first
        journal_inner_update(liter, replacement_l->get_laddr(), maybe_get_delta_buffer());
        bool overflow = extent_will_overflow(replacement_pivot.size() + 1, std::nullopt);
        if (!overflow) {
          journal_inner_replace(riter, replacement_r->get_laddr(),
                                replacement_pivot, maybe_get_delta_buffer());
          std::vector<laddr_t> dec_laddrs{l->get_laddr(), r->get_laddr()};
          return dec_ref(oc, dec_laddrs).safe_then([] {
            return merge_entry_ret(
                   merge_entry_ertr::ready_future_marker{},
                   mutation_result_t(mutation_status_t::SUCCESS, std::nullopt, std::nullopt));
          });
        } else {
          logger().debug("{}::merge_entry balanced and split {} r {}", __func__, *l, *r);
          //use remove and insert to instead of replace,
          //remove operation will not cause node split, so we can do it first
          journal_inner_remove(riter, maybe_get_delta_buffer());
          return make_split_insert(oc, riter, replacement_pivot, replacement_r->get_laddr())
            .safe_then([this, oc, l = l, r = r] (auto mresult) {
            std::vector<laddr_t> dec_laddrs{l->get_laddr(), r->get_laddr(), get_laddr()};
            return dec_ref(oc, dec_laddrs)
              .safe_then([mresult = std::move(mresult)] {
              return merge_entry_ret(
                     merge_entry_ertr::ready_future_marker{},
                     mresult);
            });
          });
        }
      });
    }
  });

}

OMapInnerNode::internal_iterator_t
OMapInnerNode::get_containing_child(const std::string &key)
{
  auto iter = std::find_if(iter_begin(), iter_end(),
       [&key](auto it) { return it.contains(key); });
  return iter;
}

std::ostream &OMapLeafNode::print_detail_l(std::ostream &out) const
{
  return out << ", size=" << get_size()
         << ", depth=" << get_meta().depth;
}

OMapLeafNode::get_value_ret
OMapLeafNode::get_value(omap_context_t oc, const std::string &key)
{
  logger().debug("OMapLeafNode: {} key = {}", __func__, key);
  auto ite = find_string_key(key);
  if (ite != iter_end()) {
    auto value = ite->get_string_val();
    return get_value_ret(
      get_value_ertr::ready_future_marker{},
      std::make_pair(key, value));
  } else {
    return get_value_ret(
      get_value_ertr::ready_future_marker{},
      std::make_pair(key, ""));
  }
}

OMapLeafNode::insert_ret
OMapLeafNode::insert(omap_context_t oc, const std::string &key, const std::string &value)
{
  logger().debug("OMapLeafNode: {}, {} -> {}", __func__, key, value);
  bool overflow = extent_will_overflow(key.size() + 1, value.size() + 1);
  if (!overflow) {
    if (!is_pending()) {
      auto mut = oc.tm.get_mutable_extent(oc.t, this)->cast<OMapLeafNode>();
      return mut->insert(oc, key, value);
    }
    auto replace_pt = find_string_key(key);
    if (replace_pt != iter_end()) {
      journal_leaf_update(replace_pt, key, value, maybe_get_delta_buffer());
    } else {
      auto insert_pt = string_lower_bound(key);
      journal_leaf_insert(insert_pt, key, value, maybe_get_delta_buffer());

      logger().debug(
        "{}: {} inserted {}->{} {}"," OMapLeafNode",  __func__,
        insert_pt.get_node_key(),
        insert_pt.get_node_val(),
        insert_pt.get_string_val());
    }
    return insert_ret(
           insert_ertr::ready_future_marker{},
           mutation_result_t(mutation_status_t::SUCCESS, std::nullopt, std::nullopt));
  } else {
    return make_split_children(oc).safe_then([this, oc, &key, &value] (auto tuple) {
      auto [left, right, pivot] = tuple;
      auto replace_pt = find_string_key(key);
      if (replace_pt != iter_end()) {
        if (key < pivot) {  //left
          auto mut_iter = left->iter_idx(replace_pt->get_index());
          left->journal_leaf_update(mut_iter, key, value, left->maybe_get_delta_buffer());
        } else if (key >= pivot) { //right
          auto mut_iter = right->iter_idx(replace_pt->get_index() - left->get_node_size());
          right->journal_leaf_update(mut_iter, key, value, right->maybe_get_delta_buffer());
        }
      } else {
        auto insert_pt = string_lower_bound(key);
        if (key < pivot) {  //left
          auto mut_iter = left->iter_idx(insert_pt->get_index());
          left->journal_leaf_insert(mut_iter, key, value, left->maybe_get_delta_buffer());
        } else {
          auto mut_iter = right->iter_idx(insert_pt->get_index() - left->get_node_size());
          right->journal_leaf_insert(mut_iter, key, value, right->maybe_get_delta_buffer());
        }
      }
      return dec_ref(oc, get_laddr())
        .safe_then([tuple = std::move(tuple)] {
        return insert_ret(
               insert_ertr::ready_future_marker{},
               mutation_result_t(mutation_status_t::WAS_SPLIT, tuple, std::nullopt));
      });
    });
  }
}

OMapLeafNode::rm_key_ret
OMapLeafNode::rm_key(omap_context_t oc, const std::string &key)
{
  logger().debug("OMapLeafNode: {} : {}", __func__, key);
  if(!is_pending()) {
    auto mut =  oc.tm.get_mutable_extent(oc.t, this)->cast<OMapLeafNode>();
    return mut->rm_key(oc, key);
  }

  auto rm_pt = find_string_key(key);
  if (rm_pt != iter_end()) {
    journal_leaf_remove(rm_pt, maybe_get_delta_buffer());
    logger().debug(
      "{}: removed {}->{} {}", __func__,
      rm_pt->get_node_key(),
      rm_pt->get_node_val(),
      rm_pt->get_string_val());
      if (extent_is_below_min()) {
        return rm_key_ret(
               rm_key_ertr::ready_future_marker{},
               mutation_result_t(mutation_status_t::NEED_MERGE, std::nullopt,
                                 this->cast<OMapNode>()));
      } else {
        return rm_key_ret(
               rm_key_ertr::ready_future_marker{},
               mutation_result_t(mutation_status_t::SUCCESS, std::nullopt, std::nullopt));
      }
  } else {
    return rm_key_ret(
           rm_key_ertr::ready_future_marker{},
           mutation_result_t(mutation_status_t::FAIL, std::nullopt, std::nullopt));
  }

}

OMapLeafNode::list_keys_ret
OMapLeafNode::list_keys(omap_context_t oc, std::string &start, size_t max_result_size)
{
  logger().debug("OMapLeafNode: {}", __func__);
  auto result = list_keys_result_t();
  iterator  iter = start == "" ?  iter_begin() : string_lower_bound(start);
  // two stop condition, reach the end of leaf or size > required size(max_result_size)
  for (; iter != iter_end() && result.keys.size() <= max_result_size; iter++) {
    result.keys.push_back(iter->get_node_val());
  }
  if (iter == iter_end())
   result.next = std::nullopt;   //have searched all items in the leaf
  else
   result.next = iter->get_node_val();

  return list_keys_ertr::make_ready_future<list_keys_result_t>(std::move(result));

}

OMapLeafNode::list_ret
OMapLeafNode::list(omap_context_t oc, std::string &start, size_t max_result_size)
{
  logger().debug("OMapLeafNode: {}", __func__);
  auto result = list_kvs_result_t();
  iterator  iter = start == "" ? iter_begin() : string_lower_bound(start);
  for (; iter != iter_end() && result.kvs.size() <= max_result_size; iter++) {
    result.kvs.push_back({iter->get_node_val(), iter->get_string_val()});
  }
  if (iter == iter_end())
   result.next = std::nullopt;  //have searched all items in the lead
  else
   result.next = iter->get_node_val();

  return list_ertr::make_ready_future<list_kvs_result_t>(std::move(result));
}

OMapLeafNode::clear_ret
OMapLeafNode::clear(omap_context_t oc)
{
  return clear_ertr::now();
}

OMapLeafNode::split_children_ret
OMapLeafNode::make_split_children(omap_context_t oc)
{
  logger().debug("OMapLeafNode: {}", __func__);
  return oc.tm.alloc_extents<OMapLeafNode>(oc.t, L_ADDR_MIN, OMAP_BLOCK_SIZE, 2)
    .safe_then([this] (auto &&ext_pair) {
      auto left = ext_pair.front();
      auto right = ext_pair.back();
      return split_children_ret(
             split_children_ertr::ready_future_marker{},
             std::make_tuple(left, right, split_into(*left, *right)));
  });
}

OMapLeafNode::full_merge_ret
OMapLeafNode::make_full_merge(omap_context_t oc, OMapNodeRef right)
{
  ceph_assert(right->get_type() == type);
  logger().debug("OMapLeafNode: {}", __func__);
  return oc.tm.alloc_extent<OMapLeafNode>(oc.t, L_ADDR_MIN, OMAP_BLOCK_SIZE)
    .safe_then([this, right] (auto &&replacement) {
      replacement->merge_from(*this, *right->cast<OMapLeafNode>());
      return full_merge_ret(
        full_merge_ertr::ready_future_marker{},
        std::move(replacement));
  });
}

OMapLeafNode::make_balanced_ret
OMapLeafNode::make_balanced(omap_context_t oc, OMapNodeRef _right)
{
  ceph_assert(_right->get_type() == type);
  logger().debug("OMapLeafNode: {}",  __func__);
  return oc.tm.alloc_extents<OMapLeafNode>(oc.t, L_ADDR_MIN, OMAP_BLOCK_SIZE, 2)
    .safe_then([this, _right] (auto &&replacement_pair) {
      auto replacement_left = replacement_pair.front();
      auto replacement_right = replacement_pair.back();
      auto &right = *_right->cast<OMapLeafNode>();
      return make_balanced_ret(
             make_balanced_ertr::ready_future_marker{},
             std::make_tuple(
               replacement_left, replacement_right,
               balance_into_new_nodes(
                 *this, right,
                 *replacement_left, *replacement_right)));
  });
}


omap_load_extent_ertr::future<OMapNodeRef>
omap_load_extent(omap_context_t oc, laddr_t laddr, depth_t depth)
{
  ceph_assert(depth > 0);
  if (depth > 1) {
    return oc.tm.read_extents<OMapInnerNode>(oc.t, laddr, OMAP_BLOCK_SIZE
    ).handle_error(
      omap_load_extent_ertr::pass_further{},
      crimson::ct_error::assert_all{ "Invalid error in omap_load_extent" }
    ).safe_then(
      [](auto&& extents) {
      assert(extents.size() == 1);
      [[maybe_unused]] auto [laddr, e] = extents.front();
      return seastar::make_ready_future<OMapNodeRef>(std::move(e));
    });
  } else {
    return oc.tm.read_extents<OMapLeafNode>(oc.t, laddr, OMAP_BLOCK_SIZE
    ).handle_error(
      omap_load_extent_ertr::pass_further{},
      crimson::ct_error::assert_all{ "Invalid error in omap_load_extent" }
    ).safe_then(
      [](auto&& extents) {
      assert(extents.size() == 1);
      [[maybe_unused]] auto [laddr, e] = extents.front();
      return seastar::make_ready_future<OMapNodeRef>(std::move(e));
    });
  }
}
}
