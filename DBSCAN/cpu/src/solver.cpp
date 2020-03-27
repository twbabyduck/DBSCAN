//
// Created by William Liu on 2020-03-18.
//

#include "solver.h"

// ctor
template <class DataType>
DBSCAN::Solver<DataType>::Solver(const std::string& input,
                                 const size_t& min_pts, const float& radius,
                                 const uint8_t& num_threads)
    : min_pts_(min_pts),
      squared_radius_(radius * radius),
      num_threads_(num_threads) {
  logger_ = spdlog::get("console");
  if (logger_ == nullptr) {
    throw std::runtime_error("logger not created!");
  }
#if defined(AVX)
  sq_rad8_ = _mm256_set_ps(squared_radius_, squared_radius_, squared_radius_,
                           squared_radius_, squared_radius_, squared_radius_,
                           squared_radius_, squared_radius_);
#endif
  using namespace std::chrono;
  high_resolution_clock::time_point start = high_resolution_clock::now();

  auto ifs = std::ifstream(input);
  ifs >> num_nodes_;
  dataset_ = std::make_unique<DataType>(num_nodes_);
  size_t n;
  float x, y;
  if (std::is_same<DataType, input_type::TwoDimPoints>::value) {
    while (ifs >> n >> x >> y) {
      dataset_->d1[n] = x;
      dataset_->d2[n] = y;
    }
  } else {
    throw std::runtime_error("Implement your own input_type!");
  }

  duration<double> time_spent =
      duration_cast<duration<double>>(high_resolution_clock::now() - start);
  logger_->info("reading vertices takes {} seconds", time_spent.count());
}

template <class DataType>
void DBSCAN::Solver<DataType>::insert_edges() {
  using namespace std::chrono;
  high_resolution_clock::time_point start = high_resolution_clock::now();

  if (dataset_ == nullptr) {
    throw std::runtime_error("Call prepare_dataset to generate the dataset!");
  }

  graph_ = std::make_unique<Graph>(num_nodes_, num_threads_);

  std::vector<std::thread> threads(num_threads_);
  const size_t chunk =
      num_nodes_ / num_threads_ + (num_nodes_ % num_threads_ != 0);
#if defined(BIT_ADJ)
  logger_->info("insert_edges - BIT_ADJ");
  const size_t N = num_nodes_ / 64u + (num_nodes_ % 64u != 0);
  for (size_t tid = 0; tid < num_threads_; ++tid) {
    threads[tid] = std::thread(
        [this, &chunk, &N](const size_t& tid) {
          auto t0 = high_resolution_clock::now();
          const size_t start = tid * chunk;
          const size_t end = std::min(start + chunk, num_nodes_);
          for (size_t u = start; u < end; ++u) {
            const float &ux = dataset_->d1[u], uy = dataset_->d2[u];
#if defined(AVX)
            __m256 const u_x8 = _mm256_set_ps(ux, ux, ux, ux, ux, ux, ux, ux);
            __m256 const u_y8 = _mm256_set_ps(uy, uy, uy, uy, uy, uy, uy, uy);
            for (size_t outer = 0; outer < N; ++outer) {
              for (size_t inner = 0; inner < 64; inner += 8) {
                const size_t v0 = outer * 64llu + inner;
                const size_t v1 = v0 + 1;
                const size_t v2 = v0 + 2;
                const size_t v3 = v0 + 3;
                const size_t v4 = v0 + 4;
                const size_t v5 = v0 + 5;
                const size_t v6 = v0 + 6;
                const size_t v7 = v0 + 7;
                // TODO: if num_nodes_ is not a multiple of 8
                // logger_->trace("node {} (num_nodes_ {}); outer{}; inner
                // {}", u, num_nodes_, outer, inner);

                float const* const v_x_ptr = &(dataset_->d1.front());
                __m256 const v_x_8 = _mm256_load_ps(v_x_ptr + v0);
                float const* const v_y_ptr = &(dataset_->d2.front());
                __m256 const v_y_8 = _mm256_load_ps(v_y_ptr + v0);
                __m256 const x_diff_8 = _mm256_sub_ps(u_x8, v_x_8);
                __m256 const x_diff_sq_8 = _mm256_mul_ps(x_diff_8, x_diff_8);
                __m256 const y_diff_8 = _mm256_sub_ps(u_y8, v_y_8);
                __m256 const y_diff_sq_8 = _mm256_mul_ps(y_diff_8, y_diff_8);
                __m256 const sum = _mm256_add_ps(x_diff_sq_8, y_diff_sq_8);

                // auto const temp = reinterpret_cast<float const*>(&sum);
                // logger_->trace("summation of X^2 and Y^2 (sum):");
                // for (size_t i = 0; i < 8; ++i)
                //   logger_->trace("\t{}", temp[i]);

                int const cmp = _mm256_movemask_ps(
                    _mm256_cmp_ps(sum, sq_rad8_, _CMP_LE_OS));
                // logger_->trace(
                //     "comparison of X^2+Y^2 against radius^2 (cmp): {}",
                //     cmp);

                if (u != v0 && v0 < num_nodes_ && (cmp & 1 << 0))
                  graph_->insert_edge(u, outer, 1llu << inner);
                if (u != v1 && v1 < num_nodes_ && (cmp & 1 << 1))
                  graph_->insert_edge(u, outer, 1llu << (inner + 1));
                if (u != v2 && v2 < num_nodes_ && (cmp & 1 << 2))
                  graph_->insert_edge(u, outer, 1llu << (inner + 2));
                if (u != v3 && v3 < num_nodes_ && (cmp & 1 << 3))
                  graph_->insert_edge(u, outer, 1llu << (inner + 3));
                if (u != v4 && v4 < num_nodes_ && (cmp & 1 << 4))
                  graph_->insert_edge(u, outer, 1llu << (inner + 4));
                if (u != v5 && v5 < num_nodes_ && (cmp & 1 << 5))
                  graph_->insert_edge(u, outer, 1llu << (inner + 5));
                if (u != v6 && v6 < num_nodes_ && (cmp & 1 << 6))
                  graph_->insert_edge(u, outer, 1llu << (inner + 6));
                if (u != v7 && v7 < num_nodes_ && (cmp & 1 << 7))
                  graph_->insert_edge(u, outer, 1llu << (inner + 7));
              }
            }
#else
            const auto dist =
                input_type::TwoDimPoints::euclidean_distance_square;
            for (size_t outer = 0; outer < N; outer += 4) {
              for (size_t inner = 0; inner < 64; ++inner) {
                const size_t v1 = outer * 64llu + inner;
                const size_t v2 = v1 + 64;
                const size_t v3 = v2 + 64;
                const size_t v4 = v3 + 64;
                const uint64_t msk = 1llu << inner;
                if (u != v1 && v1 < num_nodes_ &&
                    dist(ux, uy, dataset_->d1[v1], dataset_->d2[v1]) <=
                        squared_radius_)
                  graph_->insert_edge(u, outer, msk);
                if (u != v2 && v2 < num_nodes_ &&
                    dist(ux, uy, dataset_->d1[v2], dataset_->d2[v2]) <=
                        squared_radius_)
                  graph_->insert_edge(u, outer + 1, msk);
                if (u != v3 && v3 < num_nodes_ &&
                    dist(ux, uy, dataset_->d1[v3], dataset_->d2[v3]) <=
                        squared_radius_)
                  graph_->insert_edge(u, outer + 2, msk);
                if (u != v4 && v4 < num_nodes_ &&
                    dist(ux, uy, dataset_->d1[v4], dataset_->d2[v4]) <=
                        squared_radius_)
                  graph_->insert_edge(u, outer + 3, msk);
              }
            }
#endif
          }
          auto t1 = high_resolution_clock::now();
          logger_->info("\tThread {} takes {} seconds", tid,
                        duration_cast<duration<double>>(t1 - t0).count());
        }, /* lambda */
        tid /* args to lambda */);
  }
#else
  logger_->info("insert_edges - default");
  const auto dist = input_type::TwoDimPoints::euclidean_distance_square;
  for (size_t tid = 0; tid < num_threads_; ++tid) {
    threads[tid] = std::thread(
        [this, &dist, &chunk](const size_t& tid) {
          auto t0 = high_resolution_clock::now();
          const size_t start = tid * chunk;
          const size_t end = std::min(start + chunk, num_nodes_);
#if defined(AVX)
          // each float is 4 bytes; a 256bit register is 32 bytes. Hence 8
          // float at-a-time.
          for (size_t u = start; u < end; ++u) {
            graph_->start_insert(u);
            const float &ux = dataset_->d1[u], uy = dataset_->d2[u];
            __m256 const u_x8 = _mm256_set_ps(ux, ux, ux, ux, ux, ux, ux, ux);
            __m256 const u_y8 = _mm256_set_ps(uy, uy, uy, uy, uy, uy, uy, uy);
            // TODO: if num_nodes_ is not a multiple of 8
            for (size_t v = 0; v < num_nodes_; v += 8) {
              float const* const v_x_ptr = &(dataset_->d1.front());
              __m256 const v_x_8 = _mm256_load_ps(v_x_ptr + v);
              float const* const v_y_ptr = &(dataset_->d2.front());
              __m256 const v_y_8 = _mm256_load_ps(v_y_ptr + v);

              __m256 const x_diff_8 = _mm256_sub_ps(u_x8, v_x_8);
              __m256 const x_diff_sq_8 = _mm256_mul_ps(x_diff_8, x_diff_8);
              __m256 const y_diff_8 = _mm256_sub_ps(u_y8, v_y_8);
              __m256 const y_diff_sq_8 = _mm256_mul_ps(y_diff_8, y_diff_8);

              __m256 const sum = _mm256_add_ps(x_diff_sq_8, y_diff_sq_8);

              int const cmp =
                  _mm256_movemask_ps(_mm256_cmp_ps(sum, sq_rad8_, _CMP_LE_OS));

              if (u != v && (cmp & 1 << 0)) graph_->insert_edge(u, v);
              if (v + 1 < num_nodes_ && u != v + 1 && (cmp & 1 << 1))
                graph_->insert_edge(u, v + 1);
              if (v + 2 < num_nodes_ && u != v + 2 && (cmp & 1 << 2))
                graph_->insert_edge(u, v + 2);
              if (v + 3 < num_nodes_ && u != v + 3 && (cmp & 1 << 3))
                graph_->insert_edge(u, v + 3);
              if (v + 4 < num_nodes_ && u != v + 4 && (cmp & 1 << 4))
                graph_->insert_edge(u, v + 4);
              if (v + 5 < num_nodes_ && u != v + 5 && (cmp & 1 << 5))
                graph_->insert_edge(u, v + 5);
              if (v + 6 < num_nodes_ && u != v + 6 && (cmp & 1 << 6))
                graph_->insert_edge(u, v + 6);
              if (v + 7 < num_nodes_ && u != v + 7 && (cmp & 1 << 7))
                graph_->insert_edge(u, v + 7);
            }
            graph_->finish_insert(u);
          }
#else
          for (size_t u = start; u < end; ++u) {
            graph_->start_insert(u);
            const float &ux = dataset_->d1[u], uy = dataset_->d2[u];
            for (size_t v = 0; v < num_nodes_; ++v) {
              if (u != v && dist(ux, uy, dataset_->d1[v], dataset_->d2[v]) <=
                                squared_radius_)
                graph_->insert_edge(u, v);
            }
            graph_->finish_insert(u);
          }
#endif
          auto t1 = high_resolution_clock::now();
          logger_->info("\tThread {} takes {} seconds", tid,
                        duration_cast<duration<double>>(t1 - t0).count());
        }, /* lambda */
        tid /* args to lambda */);
  }
#endif
  for (auto& tr : threads) tr.join();
  threads.clear();

  high_resolution_clock::time_point end = high_resolution_clock::now();
  duration<double> time_spent = duration_cast<duration<double>>(end - start);
  logger_->info("insert_edges (Algorithm 1) takes {} seconds",
                time_spent.count());
}

template <class DataType>
void DBSCAN::Solver<DataType>::classify_nodes() const {
  using namespace std::chrono;
  high_resolution_clock::time_point start = high_resolution_clock::now();
  if (graph_ == nullptr) {
    throw std::runtime_error("Call insert_edges to generate the graph!");
  }
  for (size_t node = 0; node < num_nodes_; ++node) {
    // logger_->trace("{} has {} neighbours within {}", node,
    //                graph_->Va[node * 2 + 1], squared_radius_);
    // logger_->trace("{} >= {}: {}", graph_->Va[node * 2], min_pts_,
    //                graph_->Va[node * 2 + 1] >= min_pts_ ? "true" :
    //                "false");
    if (graph_->Va[node * 2 + 1] >= min_pts_) {
      // logger_->trace("{} to Core", node);
      graph_->memberships[node] = Core;
    } else {
      // logger_->trace("{} to Noise", node);
      graph_->memberships[node] = Noise;
    }
  }
  duration<double> time_spent =
      duration_cast<duration<double>>(high_resolution_clock::now() - start);
  logger_->info("classify_nodes takes {} seconds", time_spent.count());
}

template <class DataType>
void DBSCAN::Solver<DataType>::identify_cluster() const {
  using namespace std::chrono;
  high_resolution_clock::time_point start = high_resolution_clock::now();
  int cluster = 0;
  for (size_t node = 0; node < num_nodes_; ++node) {
    if (graph_->cluster_ids[node] == -1 && graph_->memberships[node] == Core) {
      graph_->cluster_ids[node] = cluster;
      // logger_->debug("start bfs on node {} with cluster {}", node,
      // cluster);
      bfs(node, cluster);
      ++cluster;
    }
  }
  duration<double> time_spent =
      duration_cast<duration<double>>(high_resolution_clock::now() - start);
  logger_->info("identify_cluster (Algorithm 2) takes {} seconds",
                time_spent.count());
}

template <class DataType>
void DBSCAN::Solver<DataType>::bfs(size_t start_node, int cluster) const {
  std::vector<size_t> curr_level{start_node};
  // each thread has its own partial frontier.
  std::vector<std::vector<size_t>> next_level(num_threads_,
                                              std::vector<size_t>());

  std::vector<std::thread> threads(num_threads_);
  // size_t lvl_cnt = 0;
  size_t chunk = 0;
  while (!curr_level.empty()) {
    chunk = curr_level.size() / num_threads_ +
            (curr_level.size() % num_threads_ != 0);
    // logger_->info("\tBFS level {}", lvl_cnt);
    for (size_t tid = 0u; tid < num_threads_; ++tid) {
      threads[tid] = std::thread(
          [this, &curr_level, &next_level, &cluster,
           &chunk](const size_t& tid) {
            // using namespace std::chrono;
            // auto p_t0 = high_resolution_clock::now();
            size_t start = tid * chunk;
            size_t end = std::min(start + chunk, curr_level.size());
            for (size_t curr_node_idx = start; curr_node_idx < end;
                 ++curr_node_idx) {
              size_t node = curr_level[curr_node_idx];
              // logger_->trace("visiting node {}", node);
              // Relabel a reachable Noise node, but do not keep exploring.
              if (graph_->memberships[node] == Noise) {
                // logger_->trace("\tnode {} is relabeled from Noise to
                // Border", node);
                graph_->memberships[node] = Border;
                continue;
              }
              size_t start_pos = graph_->Va[2 * node];
              size_t num_neighbours = graph_->Va[2 * node + 1];
              for (size_t i = 0; i < num_neighbours; ++i) {
                size_t nb = graph_->Ea[start_pos + i];
                if (graph_->cluster_ids[nb] == -1) {
                  // cluster the node
                  // logger_->trace("\tnode {} is clustered to {}", nb,
                  // cluster);
                  graph_->cluster_ids[nb] = cluster;
                  // logger_->trace("\tneighbour {} of node {} is queued", nb,
                  // node);
                  next_level[tid].emplace_back(nb);
                }
              }
            }
            // auto p_t1 = high_resolution_clock::now();
            // logger_->info(
            //     "\t\tThread {} takes {} seconds", tid,
            //     duration_cast<duration<double>>(p_t1 - p_t0).count());
          } /* lambda */,
          tid /* args to lambda */);
    }
    for (auto& tr : threads) tr.join();
    curr_level.clear();
    // sync barrier
    // flatten next_level and save to curr_level
    for (const auto& lvl : next_level)
      curr_level.insert(curr_level.end(), lvl.cbegin(), lvl.cend());
    // clear next_level
    for (auto& lvl : next_level) lvl.clear();
    // ++lvl_cnt;
  }
}

// https://stackoverflow.com/a/495056
template class DBSCAN::Solver<DBSCAN::input_type::TwoDimPoints>;