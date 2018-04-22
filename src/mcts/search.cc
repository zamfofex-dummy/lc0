/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mcts/search.h"

#include <algorithm>
#include <cmath>

#include "mcts/node.h"
#include "neural/cache.h"
#include "neural/network_tf.h"

namespace lczero {

namespace {

const int kDefaultMiniBatchSize = 16;
const char* kMiniBatchSizeOption = "Minibatch size for NN inference";

const int kDefaultPrefetchBatchSize = 64;
const char* kMiniPrefetchBatchOption = "Max prefetch nodes, per NN call";

const bool kDefaultAggresiveCaching = false;
const char* kAggresiveCachingOption = "Try hard to find what to cache";

const int kDefaultCpuct = 170;
const char* kCpuctOption = "Cpuct MCTS option (x100)";
}  // namespace

void Search::PopulateUciParams(OptionsParser* options) {
  options->Add<SpinOption>(kMiniBatchSizeOption, 1, 1024, "minibatch-size") =
      kDefaultMiniBatchSize;

  options->Add<SpinOption>(kMiniPrefetchBatchOption, 0, 1024, "max-prefetch") =
      kDefaultPrefetchBatchSize;

  options->Add<CheckOption>(kAggresiveCachingOption, "aggressive-caching") =
      kDefaultAggresiveCaching;

  options->Add<SpinOption>(kCpuctOption, 0, 9999, "cpuct") = kDefaultCpuct;
}

Search::Search(Node* root_node, NodePool* node_pool, Network* network,
               BestMoveInfo::Callback best_move_callback,
               ThinkingInfo::Callback info_callback, const SearchLimits& limits,
               const OptionsDict& options, NNCache* cache)
    : root_node_(root_node),
      node_pool_(node_pool),
      cache_(cache),
      network_(network),
      limits_(limits),
      start_time_(std::chrono::steady_clock::now()),
      initial_visits_(root_node->n),
      best_move_callback_(best_move_callback),
      info_callback_(info_callback),
      kMiniBatchSize(options.Get<int>(kMiniBatchSizeOption)),
      kMiniPrefetchBatch(options.Get<int>(kMiniPrefetchBatchOption)),
      kAggresiveCaching(options.Get<bool>(kAggresiveCachingOption)),
      kCpuct(options.Get<int>(kCpuctOption) / 100.0f) {}

// Returns whether node was already in cache.
bool Search::AddNodeToCompute(Node* node, CachingComputation* computation,
                              bool add_if_cached) {
  auto hash = node->BoardHash();
  // If already in cache, no need to do anything.
  if (add_if_cached) {
    if (computation->AddInputByHash(hash)) return true;
  } else {
    if (cache_->ContainsKey(hash)) return true;
  }
  auto planes = EncodeNode(node);

  std::vector<uint16_t> moves;

  if (node->child) {
    // Valid moves are known, using them.
    for (Node* iter = node->child; iter; iter = iter->sibling) {
      moves.emplace_back(iter->move.as_nn_index());
    }
  } else {
    // Cache pseudovalid moves. A bit of a waste, but faster.
    const auto& pseudovalid_moves = node->board.GeneratePseudovalidMoves();
    moves.reserve(pseudovalid_moves.size());
    for (const Move& m : pseudovalid_moves) {
      moves.emplace_back(m.as_nn_index());
    }
  }

  computation->AddInput(hash, std::move(planes), std::move(moves));
  return false;
}

void Search::Worker() {
  std::vector<Node*> nodes_to_process;

  // Exit check is at the end of the loop as at least one iteration is
  // necessary.
  while (true) {
    int new_nodes = 0;
    nodes_to_process.clear();
    auto computation = CachingComputation(network_->NewComputation(), cache_);

    // Gather nodes to process in the current batch.
    for (int i = 0; i < kMiniBatchSize; ++i) {
      // If there's something to do without touching slow neural net, do it.
      if (i > 0 && computation.GetCacheMisses() == 0) break;
      Node* node = PickNodeToExtend(root_node_);
      // If we hit the node that is already processed (by our batch or in
      // another thread) stop gathering and process smaller batch.
      if (!node) break;
      ++new_nodes;

      nodes_to_process.push_back(node);
      // If node is already known as terminal (win/lose/draw according to rules
      // of the game), it means that we already visited this node before.
      if (node->is_terminal) continue;

      ExtendNode(node);

      // If node turned out to be a terminal one, no need to send to NN for
      // evaluation.
      if (!node->is_terminal) {
        AddNodeToCompute(node, &computation);
      }
    }

    // If there are requests to NN, but the batch is not full, try to prefetch
    // nodes which are likely useful in future.
    if (computation.GetCacheMisses() > 0 &&
        computation.GetCacheMisses() < kMiniPrefetchBatch) {
      SharedMutex::SharedLock lock(nodes_mutex_);
      PrefetchIntoCache(root_node_,
                        kMiniPrefetchBatch - computation.GetCacheMisses(),
                        &computation);
    }

    // Evaluate nodes through NN.
    if (computation.GetBatchSize() != 0) {
      computation.ComputeBlocking();

      int idx_in_computation = 0;
      for (Node* node : nodes_to_process) {
        if (node->is_terminal) continue;
        // Populate Q value.
        node->v = -computation.GetQVal(idx_in_computation);
        // Populate P values.
        float total = 0.0;
        for (Node* n = node->child; n; n = n->sibling) {
          float p =
              computation.GetPVal(idx_in_computation, n->move.as_nn_index());
          total += p;
          n->p = p;
        }
        // Scale P values to add up to 1.0.
        if (total > 0.0f) {
          for (Node* n = node->child; n; n = n->sibling) {
            n->p /= total;
          }
        }
        ++idx_in_computation;
      }
    }

    {
      // Update nodes.
      SharedMutex::Lock lock(nodes_mutex_);
      total_playouts_ += new_nodes;
      for (Node* node : nodes_to_process) {
        float v = node->v;
        // Maximum depth the node is explored.
        uint16_t depth = 0;
        // If the node is terminal, mark it as fully explored to an infinite
        // depth.
        uint16_t cur_full_depth = node->is_terminal ? 999 : 0;
        bool full_depth_updated = true;
        for (Node* n = node; n != root_node_->parent; n = n->parent) {
          ++depth;
          // Add new value to W.
          n->w += v;
          // Increment N.
          ++n->n;
          // Decrement virtual loss.
          --n->n_in_flight;
          // Recompute Q.
          n->q = n->w / n->n;
          // Q will be flipped for opponent.
          v = -v;

          // Updating stats.
          // Max depth.
          if (depth > n->max_depth) {
            n->max_depth = depth;
          }
          // Full depth.
          if (full_depth_updated && n->full_depth <= cur_full_depth) {
            for (Node* iter = n->child; iter; iter = iter->sibling) {
              if (cur_full_depth > iter->full_depth) {
                cur_full_depth = iter->full_depth;
              }
            }
            if (cur_full_depth >= n->full_depth) {
              n->full_depth = ++cur_full_depth;
            } else {
              full_depth_updated = false;
            }
          }
          // Best move.
          if (n->parent == root_node_) {
            if (!best_move_node_ || best_move_node_->n < n->n) {
              best_move_node_ = n;
            }
          }
        }
      }
    }
    MaybeOutputInfo();
    MaybeTriggerStop();

    // If required to stop, stop.
    {
      Mutex::Lock lock(counters_mutex_);
      if (stop_) break;
    }
  }
}

// Prefetches up to @budget nodes into cache. Returns number of nodes
// prefetched.
int Search::PrefetchIntoCache(Node* node, int budget,
                              CachingComputation* computation) {
  if (budget <= 0) return 0;

  // We are in a leaf, which is not yet being processed.
  if (node->n + node->n_in_flight == 0) {
    if (AddNodeToCompute(node, computation, false)) {
      return kAggresiveCaching ? 0 : 1;
    }
    return 1;
  }

  // If it's a node in progress of expansion or is terminal, not prefetching.
  if (!node->child) return 0;

  // Populate all subnodes and their scores.
  typedef std::pair<float, Node*> ScoredNode;
  std::vector<ScoredNode> scores;
  float factor = kCpuct * std::sqrt(node->n + 1);
  for (Node* iter = node->child; iter; iter = iter->sibling) {
    scores.emplace_back(factor * iter->ComputeU() + iter->ComputeQ(), iter);
  }

  int first_unsorted_index = 0;
  int total_budget_spent = 0;
  int budget_to_spend = budget;  // Initializing for the case there's only
                                 // on child.
  for (int i = 0; i < scores.size(); ++i) {
    if (budget <= 0) break;

    // Sort next chunk of a vector. 3 of a time. Most of the times it's fine.
    if (first_unsorted_index != scores.size() &&
        i + 2 >= first_unsorted_index) {
      const int new_unsorted_index = std::min(
          static_cast<int>(scores.size()),
          budget < 2 ? first_unsorted_index + 2 : first_unsorted_index + 3);
      std::partial_sort(scores.begin() + first_unsorted_index,
                        scores.begin() + new_unsorted_index, scores.end());
      first_unsorted_index = new_unsorted_index;
    }

    Node* n = scores[i].second;
    // Last node gets the same budget as prev-to-last node.
    if (i != scores.size() - 1) {
      const float next_score = scores[i + 1].first;
      const float q = n->ComputeQ();
      if (next_score > q) {
        budget_to_spend = std::min(
            budget,
            int(n->p * factor / (next_score - q) - n->n - n->n_in_flight) + 1);
      } else {
        budget_to_spend = budget;
      }
    }
    const int budget_spent = PrefetchIntoCache(n, budget_to_spend, computation);
    budget -= budget_spent;
    total_budget_spent += budget_spent;
  }
  return total_budget_spent;
}

namespace {
// Returns a child with most visits.
Node* GetBestChild(Node* parent) {
  Node* best_node = nullptr;
  int best = -1;
  for (Node* node = parent->child; node; node = node->sibling) {
    int n = node->n + node->n_in_flight;
    if (n > best) {
      best = n;
      best_node = node;
    }
  }
  return best_node;
}
}  // namespace

void Search::SendUciInfo() REQUIRES(nodes_mutex_) {
  if (!best_move_node_) return;
  last_outputted_best_move_node_ = best_move_node_;
  uci_info_.depth = root_node_->full_depth;
  uci_info_.seldepth = root_node_->max_depth;
  uci_info_.time = GetTimeSinceStart();
  uci_info_.nodes = total_playouts_ + initial_visits_;
  uci_info_.hashfull = cache_->GetSize() * 1000LL / cache_->GetCapacity();
  uci_info_.nps =
      uci_info_.time ? (total_playouts_ * 1000 / uci_info_.time) : 0;
  uci_info_.score = -191 * log(2 / (best_move_node_->q * 0.99 + 1) - 1);
  uci_info_.pv.clear();

  for (Node* iter = best_move_node_; iter; iter = GetBestChild(iter)) {
    Move m = iter->move;
    if (!iter->board.flipped()) m.Mirror();
    uci_info_.pv.push_back(m);
  }
  uci_info_.comment.clear();
  info_callback_(uci_info_);
}

// Decides whether anything important changed in stats and new info should be
// shown to a user.
void Search::MaybeOutputInfo() {
  SharedMutex::Lock lock(nodes_mutex_);
  if (best_move_node_ && (best_move_node_ != last_outputted_best_move_node_ ||
                          uci_info_.depth != root_node_->full_depth ||
                          uci_info_.seldepth != root_node_->max_depth)) {
    SendUciInfo();
  }
}

uint64_t Search::GetTimeSinceStart() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start_time_)
      .count();
}

void Search::MaybeTriggerStop() {
  Mutex::Lock lock(counters_mutex_);
  SharedMutex::Lock nodes_lock(nodes_mutex_);
  if (limits_.playouts >= 0 && total_playouts_ >= limits_.playouts) {
    stop_ = true;
  }
  if (limits_.visits >= 0 &&
      total_playouts_ + initial_visits_ >= limits_.visits) {
    stop_ = true;
  }
  if (limits_.time_ms >= 0 && GetTimeSinceStart() >= limits_.time_ms) {
    stop_ = true;
  }
  if (stop_ && !responded_bestmove_) {
    responded_bestmove_ = true;
    SendUciInfo();
    auto best_move = GetBestMoveInternal();
    best_move_callback_({best_move.first, best_move.second});
    best_move_node_ = nullptr;
  }
}

void Search::ExtendNode(Node* node) {
  // Not taking mutex because other threads will see that N=0 and N-in-flight=1
  // and will not touch this node.
  auto& board = node->board;
  auto valid_moves = board.GenerateValidMoves();

  // Check whether it's a draw/lose by rules.
  if (valid_moves.empty()) {
    // Checkmate or stalemate.
    node->is_terminal = true;
    if (board.IsUnderCheck()) {
      // Checkmate.
      node->v = 1.0f;
    } else {
      // Stalemate.
      node->v = 0.0f;
    }
    return;
  }

  if (!board.HasMatingMaterial()) {
    node->is_terminal = true;
    node->v = 0.0f;
    return;
  }

  if (node->no_capture_ply >= 100) {
    node->is_terminal = true;
    node->v = 0.0f;
    return;
  }

  node->repetitions = node->ComputeRepetitions();
  if (node->repetitions >= 2) {
    node->is_terminal = true;
    node->v = 0.0f;
    return;
  }

  // Add valid moves as children to this node.
  Node* prev_node = node;
  for (const auto& move : valid_moves) {
    Node* new_node = node_pool_->GetNode();

    new_node->parent = node;
    if (prev_node == node) {
      node->child = new_node;
    } else {
      prev_node->sibling = new_node;
    }

    new_node->move = move.move;
    new_node->board = move.board;
    new_node->board.Mirror();
    new_node->no_capture_ply =
        move.reset_50_moves ? 0 : (node->no_capture_ply + 1);
    new_node->ply_count = node->ply_count + 1;
    prev_node = new_node;
  }
}

Node* Search::PickNodeToExtend(Node* node) {
  while (true) {
    {
      SharedMutex::Lock lock(nodes_mutex_);
      // Check whether we are in the leave.
      if (node->n == 0 && node->n_in_flight > 0) {
        // The node is currently being processed by another thread.
        // Undo the increments of anschestor nodes, and return null.
        for (node = node->parent; node != root_node_->parent;
             node = node->parent) {
          --node->n_in_flight;
        }
        return nullptr;
      }
      ++node->n_in_flight;
      // Found leave, and we are the the first to visit it.
      if (!node->child) {
        return node;
      }
    }

    // Now we are not in leave, we need to go deeper.
    SharedMutex::SharedLock lock(nodes_mutex_);
    float factor = kCpuct * std::sqrt(node->n + 1);
    float best = -100.0f;
    for (Node* iter = node->child; iter; iter = iter->sibling) {
      const float score = factor * iter->ComputeU() + iter->ComputeQ();
      if (score > best) {
        best = score;
        node = iter;
      }
    }
  }
}

InputPlanes Search::EncodeNode(const Node* node) {
  const int kMoveHistory = 8;
  const int planesPerBoard = 13;
  const int kAuxPlaneBase = planesPerBoard * kMoveHistory;

  InputPlanes result(kAuxPlaneBase + 8);

  const bool we_are_black = node->board.flipped();
  bool flip = false;

  for (int i = 0; i < kMoveHistory; ++i, flip = !flip) {
    if (!node) break;
    ChessBoard board = node->board;
    if (flip) board.Mirror();

    const int base = i * planesPerBoard;
    if (i == 0) {
      if (board.castlings().we_can_000()) result[kAuxPlaneBase + 0].SetAll();
      if (board.castlings().we_can_00()) result[kAuxPlaneBase + 1].SetAll();
      if (board.castlings().they_can_000()) result[kAuxPlaneBase + 2].SetAll();
      if (board.castlings().they_can_00()) result[kAuxPlaneBase + 3].SetAll();
      if (we_are_black) result[kAuxPlaneBase + 4].SetAll();
      result[kAuxPlaneBase + 5].Fill(node->no_capture_ply);
    }

    result[base + 0].mask = (board.ours() * board.pawns()).as_int();
    result[base + 1].mask = (board.our_knights()).as_int();
    result[base + 2].mask = (board.ours() * board.bishops()).as_int();
    result[base + 3].mask = (board.ours() * board.rooks()).as_int();
    result[base + 4].mask = (board.ours() * board.queens()).as_int();
    result[base + 5].mask = (board.our_king()).as_int();

    result[base + 6].mask = (board.theirs() * board.pawns()).as_int();
    result[base + 7].mask = (board.their_knights()).as_int();
    result[base + 8].mask = (board.theirs() * board.bishops()).as_int();
    result[base + 9].mask = (board.theirs() * board.rooks()).as_int();
    result[base + 10].mask = (board.theirs() * board.queens()).as_int();
    result[base + 11].mask = (board.their_king()).as_int();

    const int repetitions = node->repetitions;
    if (repetitions >= 1) result[base + 12].SetAll();

    node = node->parent;
  }

  return result;
}

std::pair<Move, Move> Search::GetBestMove() const {
  SharedMutex::SharedLock lock(nodes_mutex_);
  return GetBestMoveInternal();
}

std::pair<Move, Move> Search::GetBestMoveInternal() const
    REQUIRES_SHARED(nodes_mutex_) {
  if (!root_node_->child) return {};
  Node* best_node = GetBestChild(root_node_);
  Move move = best_node->move;
  if (!best_node->board.flipped()) move.Mirror();

  Move ponder_move;
  if (best_node->child) {
    ponder_move = GetBestChild(best_node)->move;
    if (best_node->board.flipped()) ponder_move.Mirror();
  }
  return {move, ponder_move};
}

void Search::StartThreads(int how_many) {
  Mutex::Lock lock(threads_mutex_);
  while (threads_.size() < how_many) {
    threads_.emplace_back([&]() { Worker(); });
  }
}

void Search::RunSingleThreaded() { Worker(); }

void Search::RunBlocking(int threads) {
  if (threads == 1) {
    Worker();
  } else {
    StartThreads(threads);
    Wait();
  }
}

void Search::Stop() {
  Mutex::Lock lock(counters_mutex_);
  stop_ = true;
}

void Search::Abort() {
  Mutex::Lock lock(counters_mutex_);
  responded_bestmove_ = true;
  stop_ = true;
}

void Search::Wait() {
  Mutex::Lock lock(threads_mutex_);
  while (!threads_.empty()) {
    threads_.back().join();
    threads_.pop_back();
  }
}

Search::~Search() {
  Abort();
  Wait();
}

}  // namespace lczero