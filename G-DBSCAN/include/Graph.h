//
// Created by William Liu on 2020-01-23.
//

#ifndef GDBSCAN_INCLUDE_GRAPH_H_
#define GDBSCAN_INCLUDE_GRAPH_H_

#include <immintrin.h>
#include <nmmintrin.h>
#include <spdlog/spdlog.h>

#include <bitset>
#include <vector>

#include "Helper.h"
#include "Membership.h"

namespace GDBSCAN {

class Graph {
 public:
  std::vector<size_t> Va;
  std::vector<size_t, GDBSCAN::helper::NonConstructAllocator<size_t>> Ea;
  std::vector<int> cluster_ids;
  std::vector<membership::Membership> membership;
#if defined(BIT_ADJ)
  explicit Graph(size_t num_nodes, size_t num_threads)
      : Va(num_nodes * 2, 0),
        // -1 as unvisited/un-clustered.
        cluster_ids(num_nodes, -1),
        membership(num_nodes, membership::Noise),
        num_nodes_(num_nodes),
        num_threads_(num_threads) {
    set_logger_();
    // uint64_t is 64 bits; ceiling division.
    size_t num_uint64 = num_nodes_ / 64u + (num_nodes_ % 64u != 0);
    temp_adj_.resize(num_nodes_, std::vector<uint64_t>(num_uint64, 0u));
  }
#else
  explicit Graph(size_t num_nodes, size_t num_threads)
      : Va(num_nodes * 2, 0),
        // -1 as unvisited/un-clustered.
        cluster_ids(num_nodes, -1),
        membership(num_nodes, membership::Noise),
        num_nodes_(num_nodes),
        num_threads_(num_threads),
        temp_adj_(num_nodes, std::vector<size_t>()) {
    set_logger_();
  }
#endif

#if defined(BIT_ADJ)
  void insert_edge(const size_t& u, const size_t& idx, const uint64_t& mask) {
    assert_mutable_();
    if (u >= num_nodes_ || idx >= temp_adj_[u].size()) {
      std::ostringstream oss;
      oss << "u=" << u << " or idx=" << idx << " is out of bound!";
      throw std::runtime_error(oss.str());
    }
    temp_adj_[u][idx] |= mask;
  }
#else
  void insert_edge(size_t u, size_t v) {
    assert_mutable_();
    if (u >= num_nodes_ || v >= num_nodes_) {
      std::ostringstream oss;
      oss << "u=" << u << " or v=" << v << " is out of bound!";
      throw std::runtime_error(oss.str());
    }
    logger_->trace("push {} as a neighbour of {}", v, u);
    temp_adj_[u].push_back(v);
  }
#endif

  void cluster_node(size_t node, int cluster_id) {
    assert_immutable_();
    if (node >= num_nodes_) {
      std::ostringstream oss;
      oss << node << " is out of bound!";
      throw std::runtime_error(oss.str());
    }
    cluster_ids[node] = cluster_id;
  }

#if defined(BIT_ADJ)
  void finalize() {
    logger_->info("finalize - BIT_ADJ");
    assert_mutable_();

    using namespace std::chrono;
    auto t0 = high_resolution_clock::now();

    // TODO: exclusive scan
    for (size_t node = 0; node < num_nodes_; ++node) {
      // position in Ea
      Va[node * 2] = node == 0 ? 0 : (Va[node * 2 - 1] + Va[node * 2 - 2]);
      for (const uint64_t& val : temp_adj_[node]) {
        // number of neighbours
        Va[node * 2 + 1] += __builtin_popcountll(val);
      }
    }

    auto t1 = high_resolution_clock::now();
    auto d1 = duration_cast<duration<double>>(t1 - t0);
    logger_->info("\tconstructing Va takes {} seconds", d1.count());

    const size_t sz = Va[Va.size() - 1] + Va[Va.size() - 2];
    // return if the graph has no edges.
    if (sz == 0u) {
      temp_adj_.clear();
      temp_adj_.shrink_to_fit();
      immutable_ = true;
      return;
    }

    Ea.resize(sz);

    auto t2 = high_resolution_clock::now();
    auto d2 = duration_cast<duration<double>>(t2 - t1);
    logger_->info("\tInit Ea takes {} seconds", d2.count());

    std::vector<std::thread> threads(num_threads_);
    for (size_t tid = 0; tid < num_threads_; ++tid) {
      logger_->debug("\tspawning thread {}", tid);
      threads[tid] = std::thread(
          [this](const size_t& tid) {
            auto start = high_resolution_clock::now();
            for (size_t u = tid; u < num_nodes_; u += num_threads_) {
              const std::vector<uint64_t>& nbs = temp_adj_[u];
              auto it = std::next(Ea.begin(), Va[2 * u]);
              for (size_t i = 0; i < nbs.size(); ++i) {
                uint64_t val = nbs[i];
                while (val) {
                  uint8_t k = __builtin_ffsll(val) - 1;
                  *it = 64 * i + k;
                  logger_->trace("k={}, *it={}", k, *it);
                  ++it;
                  val &= (val - 1);
                }
              }
              assert(std::distance(Ea.begin(), it) ==
                         Va[2 * u] + Va[2 * u + 1] &&
                     "iterator steps != Va[2*u+1]");
            }
            auto finish = high_resolution_clock::now();
            logger_->info(
                "\t\tThread {} takes {} seconds", tid,
                duration_cast<duration<double>>(finish - start).count());
          } /* lambda */,
          tid);
    }
    for (auto& tr : threads) tr.join();
    logger_->debug("\tjoined all threads");

    auto t3 = high_resolution_clock::now();
    auto d3 = duration_cast<duration<double>>(t3 - t2);
    logger_->info("\tCalc Ea takes {} seconds", d3.count());

    temp_adj_.clear();
    temp_adj_.shrink_to_fit();
    immutable_ = true;
  }
#else   // BIT_ADJ
  void finalize() {
    logger_->info("finalize - DEFAULT");
    assert_mutable_();

    using namespace std::chrono;
    auto t0 = high_resolution_clock::now();

    size_t node = 0;
    // TODO: paralleled exclusive scan
    for (const auto& nbs : temp_adj_) {
      // pos in Ea
      Va[node * 2] = node == 0 ? 0 : (Va[node * 2 - 1] + Va[node * 2 - 2]);
      // number of neighbours
      Va[node * 2 + 1] = nbs.size();
      ++node;
    }

    auto t1 = high_resolution_clock::now();
    auto d1 = duration_cast<duration<double>>(t1 - t0);
    logger_->info("\tCalc Va takes {} seconds", d1.count());

    const size_t sz = Va[Va.size() - 1] + Va[Va.size() - 2];
    // return if the graph has no edges.
    if (sz == 0u) {
      temp_adj_.clear();
      temp_adj_.shrink_to_fit();
      immutable_ = true;
      return;
    }

    Ea.resize(sz);

    auto t2 = high_resolution_clock::now();
    auto d2 = duration_cast<duration<double>>(t2 - t1);
    logger_->info("\tInit Ea takes {} seconds", d2.count());

    std::vector<std::thread> threads(num_threads_);
    for (size_t tid = 0; tid < num_threads_; ++tid) {
      logger_->debug("\tspawning thread {}", tid);
      threads[tid] = std::thread(
          [this](const size_t& tid) {
            auto start = high_resolution_clock::now();
            for (size_t u = tid; u < num_nodes_; u += num_threads_) {
              const auto& nbs = temp_adj_[u];
              logger_->trace("\twriting vtx {} with # nbs {}", u, nbs.size());
              assert(nbs.size() == Va[2 * u + 1] && "nbs.size!=Va[2*u+1]");
              std::copy(nbs.cbegin(), nbs.cend(), Ea.begin() + Va[2 * u]);
            }
            auto finish = high_resolution_clock::now();
            logger_->info(
                "\t\tThread {} takes {} seconds", tid,
                duration_cast<duration<double>>(finish - start).count());
          }, /* lambda */
          tid /* args to lambda */);
    }
    for (auto& tr : threads) tr.join();
    logger_->debug("\tjoined all threads");

    auto t3 = high_resolution_clock::now();
    auto d3 = duration_cast<duration<double>>(t3 - t2);
    logger_->info("\tCalc Ea takes {} seconds", d3.count());

    temp_adj_.clear();
    temp_adj_.shrink_to_fit();
    immutable_ = true;
  }
#endif  // BIT_ADJ

 private:
  bool immutable_ = false;
  size_t num_nodes_;
  size_t num_threads_;
  std::shared_ptr<spdlog::logger> logger_ = nullptr;
#if defined(BIT_ADJ)
  std::vector<std::vector<uint64_t>> temp_adj_;
#else
  std::vector<std::vector<size_t>> temp_adj_;
#endif

  void constexpr assert_mutable_() const {
    if (immutable_) {
      throw std::runtime_error("Graph is immutable!");
    }
  }
  void constexpr assert_immutable_() const {
    if (!immutable_) {
      throw std::runtime_error("finalize is not called on graph!");
    }
  }
  void set_logger_() {
    logger_ = spdlog::get("console");
    if (logger_ == nullptr) {
      throw std::runtime_error("logger not created!");
    }
  }
};
}  // namespace GDBSCAN

#endif  // GDBSCAN_INCLUDE_GRAPH_H_
