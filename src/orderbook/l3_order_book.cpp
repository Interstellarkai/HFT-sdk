// ============================================================================
// HFT-sdk — L3 Order Book Implementation
// ============================================================================

#include "l3_order_book.hpp"

#include <algorithm>
#include <cstring>

using namespace HFT_sdk;

L3OrderBook::L3OrderBook(const Symbol& symbol) : symbol_(symbol) {}

// ── Add Order ──────────────────────────────────────────────────────────────
ExecutionReport L3OrderBook::add_order(const Order& order, Timestamp ts) {
  ExecutionReport rpt{};
  rpt.order_id = order.id;
  rpt.trader_id = order.trader_id;
  rpt.symbol = order.symbol;
  rpt.side = order.side;
  rpt.type = order.type;
  rpt.price = order.price;
  rpt.order_qty = order.quantity;
  rpt.transact_time = ts;
  std::memcpy(rpt.cl_ord_id, order.cl_ord_id, sizeof(rpt.cl_ord_id));

  // Check duplicate
  if (order_index_.count(order.id)) {
    rpt.status = OrderStatus::Rejected;
    rpt.reject_reason = RejectReason::DuplicateOrderId;
    rpt.leaves_qty = 0;
    return rpt;
  }

  // Attempt matching
  auto result = match_incoming(order, ts);

  Quantity filled = order.quantity - result.remaining_qty;
  rpt.filled_qty = filled;
  rpt.leaves_qty = result.remaining_qty;

  if (result.remaining_qty == 0) {
    rpt.status = OrderStatus::Filled;
  } else if (filled > 0) {
    rpt.status = OrderStatus::PartiallyFilled;
    if (!result.trades.empty()) {
      rpt.last_qty = result.trades.back().qty;
      rpt.last_price = result.trades.back().price;
    }
  } else {
    rpt.status = OrderStatus::New;
  }

  // Rest remaining quantity on the book (for Limit + Day/GTC)
  if (result.remaining_qty > 0 && order.type == OrderType::Limit) {
    if (order.tif == TimeInForce::Day || order.tif == TimeInForce::GTC) {
      add_resting_order(order, result.remaining_qty, ts);
    } else if (order.tif == TimeInForce::IOC) {
      // IOC: cancel remainder, already partially filled above
      if (filled > 0)
        rpt.status = OrderStatus::PartiallyFilled;
      else
        rpt.status = OrderStatus::Canceled;
      rpt.leaves_qty = 0;
    } else if (order.tif == TimeInForce::FOK) {
      // FOK: reject if not fully filled (trades should have been rolled back)
      rpt.status = OrderStatus::Rejected;
      rpt.reject_reason = RejectReason::InsufficientLiquidity;
      rpt.filled_qty = 0;
      rpt.leaves_qty = 0;
    }
  }

  // Publish market data updates
  if (!result.trades.empty() || rpt.status == OrderStatus::New) {
    publish_tob(ts);
  }

  return rpt;
}

// ── Cancel Order ───────────────────────────────────────────────────────────
ExecutionReport L3OrderBook::cancel_order(OrderId id, Timestamp ts) {
  ExecutionReport rpt{};
  rpt.order_id = id;
  rpt.transact_time = ts;

  auto it = order_index_.find(id);
  if (it == order_index_.end()) {
    rpt.status = OrderStatus::Rejected;
    rpt.reject_reason = RejectReason::None;
    return rpt;
  }

  auto& loc = it->second;
  auto& l3_order = *(loc.it);

  rpt.trader_id = l3_order.trader_id;
  rpt.symbol = symbol_;
  rpt.side = l3_order.side;
  rpt.price = l3_order.price;
  rpt.order_qty =
      l3_order.visible_qty + l3_order.hidden_qty + l3_order.filled_qty;
  rpt.filled_qty = l3_order.filled_qty;
  rpt.leaves_qty = 0;
  rpt.status = OrderStatus::Canceled;
  std::memcpy(rpt.cl_ord_id, l3_order.cl_ord_id, sizeof(rpt.cl_ord_id));

  // Remove from book
  Price price = loc.price;
  if (loc.side == Side::Buy) {
    auto level_it = bids_.find(price);
    if (level_it != bids_.end()) {
      level_it->second.erase(loc.it);
      if (level_it->second.empty())
        bids_.erase(level_it);
      else
        update_queue_positions(Side::Buy, price);
    }
    --bid_order_count_;
  } else {
    auto level_it = asks_.find(price);
    if (level_it != asks_.end()) {
      level_it->second.erase(loc.it);
      if (level_it->second.empty())
        asks_.erase(level_it);
      else
        update_queue_positions(Side::Sell, price);
    }
    --ask_order_count_;
  }

  order_index_.erase(it);
  publish_tob(ts);
  return rpt;
}

// ── Replace Order ──────────────────────────────────────────────────────────
ExecutionReport L3OrderBook::replace_order(const ReplaceRequest& req,
                                           Timestamp ts) {
  // Cancel-replace: cancel old, submit new
  auto cancel_rpt = cancel_order(req.order_id, ts);
  if (cancel_rpt.status != OrderStatus::Canceled) {
    cancel_rpt.status = OrderStatus::Rejected;
    return cancel_rpt;
  }

  Order new_order{};
  new_order.id = req.order_id;  // reuse ID or generate new
  new_order.trader_id = req.trader_id;
  new_order.symbol = req.symbol;
  new_order.side = cancel_rpt.side;
  new_order.type = OrderType::Limit;
  new_order.tif = TimeInForce::Day;
  new_order.price = req.new_price;
  new_order.quantity = req.new_qty;
  new_order.submit_time = ts;
  std::memcpy(new_order.cl_ord_id, req.cl_ord_id, sizeof(new_order.cl_ord_id));

  return add_order(new_order, ts);
}

// ── Matching Logic ─────────────────────────────────────────────────────────
MatchResult L3OrderBook::match_incoming(const Order& order, Timestamp ts) {
  MatchResult result;
  result.remaining_qty = order.quantity;

  // FOK check: can we fill the entire quantity?
  if (order.tif == TimeInForce::FOK) {
    Quantity available = 0;
    if (order.side == Side::Buy) {
      for (auto& [price, queue] : asks_) {
        if (order.type == OrderType::Limit && price > order.price) break;
        for (auto& o : queue) available += o.visible_qty + o.hidden_qty;
        if (available >= order.quantity) break;
      }
    } else {
      for (auto& [price, queue] : bids_) {
        if (order.type == OrderType::Limit && price < order.price) break;
        for (auto& o : queue) available += o.visible_qty + o.hidden_qty;
        if (available >= order.quantity) break;
      }
    }
    if (available < order.quantity) {
      result.fully_filled = false;
      return result;  // FOK cannot fill
    }
  }

  if (order.side == Side::Buy) {
    match_against(asks_, order, result.remaining_qty, result.trades, ts);
  } else {
    match_against(bids_, order, result.remaining_qty, result.trades, ts);
  }

  result.fully_filled = (result.remaining_qty == 0);
  return result;
}

template <typename PriceLevels>
void L3OrderBook::match_against(PriceLevels& levels, const Order& incoming,
                                Quantity& remaining, std::vector<Trade>& trades,
                                Timestamp ts) {
  auto level_it = levels.begin();
  while (remaining > 0 && level_it != levels.end()) {
    Price level_price = level_it->first;

    // Price check for limit orders
    if (incoming.type == OrderType::Limit) {
      if (incoming.side == Side::Buy && level_price > incoming.price) break;
      if (incoming.side == Side::Sell && level_price < incoming.price) break;
    }

    auto& queue = level_it->second;
    auto order_it = queue.begin();
    while (remaining > 0 && order_it != queue.end()) {
      auto& resting = *order_it;
      Quantity resting_available = resting.visible_qty + resting.hidden_qty;
      Quantity match_qty = std::min(remaining, resting_available);

      // Create trade
      Trade trade;
      trade.trade_id = next_trade_id_++;
      trade.resting_id = resting.id;
      trade.incoming_id = incoming.id;
      trade.resting_trader = resting.trader_id;
      trade.incoming_trader = incoming.trader_id;
      trade.symbol = symbol_;
      trade.aggressor_side = incoming.side;
      trade.price = level_price;
      trade.qty = match_qty;
      trade.match_timestamp_ns = ts;

      trades.push_back(trade);
      trade_count_++;
      total_volume_ += match_qty;
      last_trade_price_ = level_price;

      if (trade_cb_) trade_cb_(trade);

      remaining -= match_qty;
      resting.filled_qty += match_qty;

      // Reduce resting order quantity
      if (match_qty <= resting.visible_qty) {
        resting.visible_qty -= match_qty;
      } else {
        Quantity from_hidden = match_qty - resting.visible_qty;
        resting.visible_qty = 0;
        resting.hidden_qty -= from_hidden;
      }

      // Iceberg replenishment
      if (resting.visible_qty == 0 && resting.hidden_qty > 0 &&
          resting.is_iceberg) {
        Quantity replenish =
            std::min(resting.hidden_qty, static_cast<Quantity>(100));
        resting.visible_qty = replenish;
        resting.hidden_qty -= replenish;
      }

      if (resting.visible_qty + resting.hidden_qty == 0) {
        // Order fully filled — remove
        order_index_.erase(resting.id);
        if (resting.side == Side::Buy)
          --bid_order_count_;
        else
          --ask_order_count_;
        order_it = queue.erase(order_it);
      } else {
        ++order_it;
      }
    }

    if (queue.empty()) {
      level_it = levels.erase(level_it);
    } else {
      ++level_it;
    }
  }
}

void L3OrderBook::add_resting_order(const Order& order, Quantity remaining,
                                    Timestamp ts) {
  L3Order l3{};
  l3.id = order.id;
  l3.trader_id = order.trader_id;
  l3.side = order.side;
  l3.price = order.price;
  l3.visible_qty = remaining;
  l3.hidden_qty = 0;
  l3.filled_qty = order.quantity - remaining;
  l3.entry_time = ts;
  l3.is_iceberg = false;
  std::memcpy(l3.cl_ord_id, order.cl_ord_id, sizeof(l3.cl_ord_id));

  if (order.side == Side::Buy) {
    auto& queue = bids_[order.price];
    queue.push_back(l3);
    auto it = std::prev(queue.end());
    it->queue_pos = static_cast<std::uint32_t>(queue.size() - 1);
    order_index_[order.id] = {Side::Buy, order.price, it};
    ++bid_order_count_;
  } else {
    auto& queue = asks_[order.price];
    queue.push_back(l3);
    auto it = std::prev(queue.end());
    it->queue_pos = static_cast<std::uint32_t>(queue.size() - 1);
    order_index_[order.id] = {Side::Sell, order.price, it};
    ++ask_order_count_;
  }
}

void L3OrderBook::update_queue_positions(Side side, Price price) {
  if (side == Side::Buy) {
    auto it = bids_.find(price);
    if (it == bids_.end()) return;
    std::uint32_t pos = 0;
    for (auto& o : it->second) o.queue_pos = pos++;
  } else {
    auto it = asks_.find(price);
    if (it == asks_.end()) return;
    std::uint32_t pos = 0;
    for (auto& o : it->second) o.queue_pos = pos++;
  }
}

void L3OrderBook::publish_tob(Timestamp ts) {
  if (tob_cb_) {
    tob_cb_(top_of_book(ts));
  }
}

// ── Query Methods ──────────────────────────────────────────────────────────
std::optional<BookLevel> L3OrderBook::best_bid() const {
  if (bids_.empty()) return std::nullopt;
  auto& [price, queue] = *bids_.begin();
  Quantity total = 0;
  std::uint32_t count = 0;
  for (auto& o : queue) {
    total += o.visible_qty;
    ++count;
  }
  return BookLevel{price, total, count};
}

std::optional<BookLevel> L3OrderBook::best_ask() const {
  if (asks_.empty()) return std::nullopt;
  auto& [price, queue] = *asks_.begin();
  Quantity total = 0;
  std::uint32_t count = 0;
  for (auto& o : queue) {
    total += o.visible_qty;
    ++count;
  }
  return BookLevel{price, total, count};
}

TopOfBook L3OrderBook::top_of_book(Timestamp ts) const {
  TopOfBook tob{};
  tob.symbol = symbol_;
  tob.timestamp = ts;

  auto bb = best_bid();
  auto ba = best_ask();

  if (bb && ba) {
    tob.valid = true;
    tob.best_bid = *bb;
    tob.best_ask = *ba;
    tob.mid_price = (bb->price + ba->price) / 2;
    tob.spread = ba->price - bb->price;

    // Microprice: size-weighted mid
    Quantity total_qty = bb->qty + ba->qty;
    if (total_qty > 0) {
      tob.micro_price = (bb->price * ba->qty + ba->price * bb->qty) / total_qty;
    } else {
      tob.micro_price = tob.mid_price;
    }
  } else if (bb) {
    tob.valid = true;
    tob.best_bid = *bb;
    tob.mid_price = bb->price;
  } else if (ba) {
    tob.valid = true;
    tob.best_ask = *ba;
    tob.mid_price = ba->price;
  }

  return tob;
}

std::vector<BookLevel> L3OrderBook::bid_depth(std::size_t levels) const {
  std::vector<BookLevel> result;
  result.reserve(levels);
  std::size_t n = 0;
  for (auto& [price, queue] : bids_) {
    if (n >= levels) break;
    Quantity total = 0;
    std::uint32_t count = 0;
    for (auto& o : queue) {
      total += o.visible_qty;
      ++count;
    }
    result.push_back({price, total, count});
    ++n;
  }
  return result;
}

std::vector<BookLevel> L3OrderBook::ask_depth(std::size_t levels) const {
  std::vector<BookLevel> result;
  result.reserve(levels);
  std::size_t n = 0;
  for (auto& [price, queue] : asks_) {
    if (n >= levels) break;
    Quantity total = 0;
    std::uint32_t count = 0;
    for (auto& o : queue) {
      total += o.visible_qty;
      ++count;
    }
    result.push_back({price, total, count});
    ++n;
  }
  return result;
}

std::vector<L3Order> L3OrderBook::orders_at_price(Side side,
                                                  Price price) const {
  std::vector<L3Order> result;
  if (side == Side::Buy) {
    auto it = bids_.find(price);
    if (it != bids_.end()) {
      result.assign(it->second.begin(), it->second.end());
    }
  } else {
    auto it = asks_.find(price);
    if (it != asks_.end()) {
      result.assign(it->second.begin(), it->second.end());
    }
  }
  return result;
}

std::uint32_t L3OrderBook::queue_position(OrderId id) const {
  auto it = order_index_.find(id);
  if (it == order_index_.end())
    return std::numeric_limits<std::uint32_t>::max();
  return it->second.it->queue_pos;
}

Quantity L3OrderBook::quantity_ahead(OrderId id) const {
  auto it = order_index_.find(id);
  if (it == order_index_.end()) return 0;

  auto& loc = it->second;
  Quantity ahead = 0;

  if (loc.side == Side::Buy) {
    auto level_it = bids_.find(loc.price);
    if (level_it != bids_.end()) {
      for (auto& o : level_it->second) {
        if (o.id == id) break;
        ahead += o.visible_qty;
      }
    }
  } else {
    auto level_it = asks_.find(loc.price);
    if (level_it != asks_.end()) {
      for (auto& o : level_it->second) {
        if (o.id == id) break;
        ahead += o.visible_qty;
      }
    }
  }

  return ahead;
}

// Explicit template instantiations
template void L3OrderBook::match_against<L3OrderBook::BidPriceLevels>(
    BidPriceLevels&, const Order&, Quantity&, std::vector<Trade>&, Timestamp);
template void L3OrderBook::match_against<L3OrderBook::AskPriceLevels>(
    AskPriceLevels&, const Order&, Quantity&, std::vector<Trade>&, Timestamp);
