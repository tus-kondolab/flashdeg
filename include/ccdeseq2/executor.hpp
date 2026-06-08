#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "ccdeseq2/workspace.hpp"

namespace ccdeseq2 {

struct GeneBlock {
  std::size_t begin = 0;
  std::size_t end = 0;
};

[[nodiscard]] std::size_t effective_thread_count(int requested_threads);
[[nodiscard]] std::size_t default_gene_block_size(std::size_t num_genes,
                                                  std::size_t threads);
[[nodiscard]] std::vector<GeneBlock> make_gene_blocks(std::size_t num_genes,
                                                      std::size_t block_size);

class GeneBlockExecutor {
 public:
  GeneBlockExecutor(int requested_threads, bool deterministic);

  [[nodiscard]] std::size_t threads() const noexcept { return threads_; }
  [[nodiscard]] bool deterministic() const noexcept { return deterministic_; }

  void run(std::size_t num_genes,
           const std::function<void(GeneBlock)>& task) const;
  void run_with_workspace(
      std::size_t num_genes,
      const std::function<void(GeneBlock, ThreadWorkspace&)>& task) const;

 private:
  std::size_t threads_;
  bool deterministic_;
};

}  // namespace ccdeseq2
