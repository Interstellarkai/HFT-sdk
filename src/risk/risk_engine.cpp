// ============================================================================
// HFT-sdk — Risk Engine Implementation
// ============================================================================

#include "risk_engine.hpp"

#include <cstdlib>

using namespace HFT_sdk;

RiskEngine::RiskEngine() = default;

void RiskEngine::setTraderLimits(TraderId trader_id,
                                 const TraderRiskLimits& limits) {
  trader_limits_[trader_id] = limits;
}

void RiskEngine::setDefaultLimits(const TraderRiskLimits& limits) {
  default_limits_ = limits;
}

RejectReason RiskEngine::check_order(const Order& order) {
  orders_checked_++;

  // 1. Kill switch
  auto r = check_kill_switch(order.trader_id);
  if (r != RejectReason::None) {
    orders_rejected_++;
    return r;
  }

  // 2. Price validity
  r = check_price_validity(order);
  if (r != RejectReason::None) {
    orders_rejected_++;
    return r;
  }

  // 3. Quantity validity
  r = check_quantity_validity(order);
  if (r != RejectReason::None) {
    orders_rejected_++;
    return r;
  }

  // 4. Position limit
  r = check_position_limit(order.trader_id, order);
  if (r != RejectReason::None) {
    orders_rejected_++;
    return r;
  }

  // 5. Rate limit
  r = check_rate_limit(order.trader_id, order.submit_time);
  if (r != RejectReason::None) {
    orders_rejected_++;
    return r;
  }

  return RejectReason::None;
}

void RiskEngine::on_fill(TraderId trader_id, const Symbol& symbol, Side side,
                         Quantity qty, Price /*price*/) {
  auto& state = get_state(trader_id);
  if (side == Side::Buy) {
    state.positions[symbol] += qty;
    state.net_position += qty;
  } else {
    state.positions[symbol] -= qty;
    state.net_position -= qty;
  }
}

void RiskEngine::kill_trader(TraderId trader_id) {
  get_state(trader_id).killed = true;
}

void RiskEngine::kill_all() {
  global_kill_.store(true, std::memory_order_release);
}

void RiskEngine::unkill_trader(TraderId trader_id) {
  get_state(trader_id).killed = false;
}

void RiskEngine::unkill_all() {
  global_kill_.store(false, std::memory_order_release);
  for (auto& [id, state] : trader_states_) {
    state.killed = false;
  }
}

bool RiskEngine::is_killed(TraderId trader_id) const {
  if (global_kill_.load(std::memory_order_relaxed)) return true;
  auto it = trader_states_.find(trader_id);
  if (it == trader_states_.end()) return false;
  return it->second.killed;
}

Quantity RiskEngine::get_position(TraderId trader_id,
                                  const Symbol& symbol) const {
  auto it = trader_states_.find(trader_id);
  if (it == trader_states_.end()) return 0;
  auto pos_it = it->second.positions.find(symbol);
  if (pos_it == it->second.positions.end()) return 0;
  return pos_it->second;
}

// ── Private helpers ────────────────────────────────────────────────────────

const TraderRiskLimits& RiskEngine::get_limits(TraderId trader_id) const {
  auto it = trader_limits_.find(trader_id);
  if (it != trader_limits_.end()) return it->second;
  return default_limits_;
}

TraderRiskState& RiskEngine::get_state(TraderId trader_id) {
  return trader_states_[trader_id];
}

RejectReason RiskEngine::check_kill_switch(TraderId trader_id) const {
  if (global_kill_.load(std::memory_order_relaxed))
    return RejectReason::KillSwitchActive;
  if (is_killed(trader_id)) return RejectReason::KillSwitchActive;
  return RejectReason::None;
}

RejectReason RiskEngine::check_position_limit(TraderId trader_id,
                                              const Order& order) {
  auto& limits = get_limits(trader_id);
  auto& state = get_state(trader_id);

  Quantity current_pos = 0;
  auto it = state.positions.find(order.symbol);
  if (it != state.positions.end()) current_pos = it->second;

  Quantity projected_pos;
  if (order.side == Side::Buy) {
    projected_pos = current_pos + order.quantity;
  } else {
    projected_pos = current_pos - order.quantity;
  }

  // Check per-symbol position limit
  if (std::abs(projected_pos) > limits.max_position_per_symbol) {
    return RejectReason::PositionLimit;
  }

  // Check net position limit
  Quantity projected_net;
  if (order.side == Side::Buy) {
    projected_net = state.net_position + order.quantity;
  } else {
    projected_net = state.net_position - order.quantity;
  }

  if (std::abs(projected_net) > limits.max_net_position) {
    return RejectReason::PositionLimit;
  }

  return RejectReason::None;
}

RejectReason RiskEngine::check_rate_limit(TraderId trader_id, Timestamp now) {
  auto& limits = get_limits(trader_id);
  auto& state = get_state(trader_id);

  // Convert window to nanoseconds
  std::uint64_t window_ns =
      static_cast<std::uint64_t>(limits.rate_window_ms) * 1'000'000ULL;

  // Remove expired timestamps
  while (!state.order_timestamps.empty() &&
         (now - state.order_timestamps.front()) > window_ns) {
    state.order_timestamps.pop_front();
  }

  if (state.order_timestamps.size() >= limits.max_order_rate) {
    return RejectReason::RateLimit;
  }

  state.order_timestamps.push_back(now);
  return RejectReason::None;
}

RejectReason RiskEngine::check_self_trade(TraderId /*trader_id*/,
                                          const Order& /*order*/) const {
  // Self-trade prevention would require access to the book.
  // In this architecture, the matching engine handles STP at match time.
  return RejectReason::None;
}

RejectReason RiskEngine::check_price_validity(const Order& order) const {
  if (order.type == OrderType::Limit && order.price <= 0) {
    return RejectReason::InvalidPrice;
  }
  return RejectReason::None;
}

RejectReason RiskEngine::check_quantity_validity(const Order& order) const {
  if (order.quantity <= 0) {
    return RejectReason::InvalidQuantity;
  }
  return RejectReason::None;
}
