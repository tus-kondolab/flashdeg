#include "ccdeseq2/executor.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace ccdeseq2 {

std::size_t effective_thread_count(int requested_threads) {
  if (requested_threads == 1) {
    return 1;
  }
  if (requested_threads > 1) {
    return static_cast<std::size_t>(requested_threads);
  }
  const unsigned int detected = std::thread::hardware_concurrency();
  return detected == 0 ? 1 : static_cast<std::size_t>(detected);
}

std::size_t default_gene_block_size(std::size_t num_genes, std::size_t threads) {
  const std::size_t effective_threads = std::max<std::size_t>(threads, 1);
  const std::size_t denominator = effective_threads * 8;
  const std::size_t ceil_block = (num_genes + denominator - 1) / denominator;
  return std::max<std::size_t>(64, ceil_block);
}

std::vector<GeneBlock> make_gene_blocks(std::size_t num_genes,
                                        std::size_t block_size) {
  block_size = std::max<std::size_t>(block_size, 1);
  std::vector<GeneBlock> blocks;
  for (std::size_t begin = 0; begin < num_genes; begin += block_size) {
    blocks.push_back({begin, std::min(begin + block_size, num_genes)});
  }
  return blocks;
}

GeneBlockExecutor::GeneBlockExecutor(int requested_threads, bool deterministic)
    : threads_(deterministic ? 1 : effective_thread_count(requested_threads)),
      deterministic_(deterministic) {}

void GeneBlockExecutor::run(std::size_t num_genes,
                            const std::function<void(GeneBlock)>& task) const {
  run_with_workspace(num_genes,
                     [&](GeneBlock block, ThreadWorkspace&) { task(block); });
}

void GeneBlockExecutor::run_with_workspace(
    std::size_t num_genes,
    const std::function<void(GeneBlock, ThreadWorkspace&)>& task) const {
  const std::size_t block_size = default_gene_block_size(num_genes, threads_);
  const std::vector<GeneBlock> blocks = make_gene_blocks(num_genes, block_size);
  const bool sequential = threads_ <= 1 || blocks.size() <= 1;

  std::vector<ThreadWorkspace> workspaces(sequential ? 1 : threads_);

  if (sequential) {
    for (const auto& block : blocks) {
      task(block, workspaces.front());
    }
    return;
  }

  std::size_t next_block = 0;
  std::mutex mutex;
  std::exception_ptr first_exception;
  std::vector<std::thread> workers;
  workers.reserve(threads_);
  for (std::size_t i = 0; i < threads_; ++i) {
    workers.emplace_back([&, i]() {
      ThreadWorkspace& workspace = workspaces[i];
      while (true) {
        GeneBlock block;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (first_exception != nullptr) {
            return;
          }
          if (next_block >= blocks.size()) {
            return;
          }
          block = blocks[next_block++];
        }
        try {
          task(block, workspace);
        } catch (...) {
          std::lock_guard<std::mutex> lock(mutex);
          if (first_exception == nullptr) {
            first_exception = std::current_exception();
          }
          return;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  if (first_exception != nullptr) {
    std::rethrow_exception(first_exception);
  }
}

}  // namespace ccdeseq2
