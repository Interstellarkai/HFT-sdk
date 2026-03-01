#pragma once
// ============================================================================
// HFT-sdk — Limit Order Book (Simple)
// Price-time priority order book with O(1) cancel via order-index.
// Used as a lightweight alternative to L3OrderBook for basic simulations.
// ============================================================================

#include <deque>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "types.hpp"

namespace HFT_sdk {

class OrderBook {
 public:
  explicit OrderBook(Symbol symbol);

  const Symbol& symbol() const noexcept { return symbol_; }

  // Add a new resting order (no matching logic here)
  void addOrder(const BookOrder& ord);

  // Reduce or remove an existing order by id - O(1) lookup via index
  bool cancelOrder(OrderId id);

  // Match an incoming order against the book
  // Returns list of trades and leftover quantity (if any).
  std::pair<std::vector<Trade>*, Quantity> matchIncoming(
      const BookOrder& incoming, std::uint64_t ts_ns);

  // Query best bid / ask
  std::optional<BookLevel> bestBid() const;
  std::optional<BookLevel> bestAsk() const;

  // Depth snapshot (up to depth levels)
  std::vector<BookLevel> bids(std::size_t depth) const;
  std::vector<BookLevel> asks(std::size_t depth) const;

 private:
  Symbol symbol_;

  // Price -> queue of orders (time-priority per price)
  // For bids: highest price first; for asks: lowest price first.
  using Queue = std::list<BookOrder>;  // linked list
  using PriceLevelBid = std::map<Price, Queue, std::greater<Price>>;
  using PriceLevelAsk = std::map<Price, Queue, std::less<Price>>;

  PriceLevelBid bids_;  // best bid at begin()
  PriceLevelAsk asks_;  // best ask at begin()

  // TODO Helper: locate order by id (linear scan for demo; could be indexed)
  bool removeFromSide(OrderId id, PriceLevelBid& side);
  bool removeFromSide(OrderId id, PriceLevelAsk& side);

  // Index for O(1) order lookup: OrderId -> (Side, Price, iterator into Queue)
  struct OrderLocation {
    Side side;
    Price price;
    Queue::iterator it;  // linked list
  };

  std::unordered_map<OrderId, OrderLocation> order_index_;
};

}  // namespace HFT_sdk
