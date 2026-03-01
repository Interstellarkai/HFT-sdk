#pragma once
// ============================================================================
// HFT-sdk — Market Data Publisher (Callback-based)
// Lightweight event-driven publisher for top-of-book, trade, and
// depth-snapshot callbacks. Decoupled from matching engine internals.
// ============================================================================

#include <functional>
#include <vector>

#include "types.hpp"

namespace HFT_sdk {

class MarketDataPublisher {
 public:
  using TopOfBookHandler = std::function<void(const TopOfBook&)>;
  using TradeHandler = std::function<void(const Trade&)>;
  using DepthSnapshotHandler =
      std::function<void(const SymbolId&, const std::vector<BookLevel>& bids,
                         const std::vector<BookLevel>& asks)>;

  void onTopOfBook(TopOfBookHandler cb);
  void onTrade(TradeHandler cb);
  void onDepthSnapshot(DepthSnapshotHandler cb);
  void publishTopOfBook(const TopOfBook& tob) const;
  void publishTrade(const Trade& t) const;
  void publishDepth(const SymbolId& sym, const std::vector<BookLevel>& bids,
                    const std::vector<BookLevel>& asks) const;

 private:
  TopOfBookHandler tob_handler_;
  TradeHandler trade_handler_;
  DepthSnapshotHandler depth_handler_;
};

}  // namespace HFT_sdk
