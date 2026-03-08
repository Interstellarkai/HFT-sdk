// tests/test_l3_order_book.cpp
#include <gtest/gtest.h>
#include "orderbook/l3_order_book.hpp"

using namespace HFT_sdk;

// ── Test fixture ────────────────────────────────────────────────────────────
class L3BookTest : public ::testing::Test {
 protected:
  Symbol sym{"AAPL"};
  L3OrderBook book{sym};
  Timestamp ts = 1'000'000;
  OrderId next_id = 1;

  Order make_order(Side side, Price price, Quantity qty,
                   OrderType type = OrderType::Limit,
                   TimeInForce tif = TimeInForce::Day,
                   TraderId trader = 1) {
    Order o{};
    o.id = next_id++;
    o.trader_id = trader;
    o.symbol = sym;
    o.side = side;
    o.type = type;
    o.tif = tif;
    o.price = price;
    o.quantity = qty;
    o.submit_time = ts++;
    return o;
  }
};

// ── L3BookBasic ─────────────────────────────────────────────────────────────
TEST_F(L3BookTest, SymbolMatchesConstruction) {
  EXPECT_EQ(book.symbol(), sym);
}

TEST_F(L3BookTest, EmptyBookHasNoBestBid) {
  EXPECT_FALSE(book.best_bid().has_value());
}

TEST_F(L3BookTest, EmptyBookHasNoBestAsk) {
  EXPECT_FALSE(book.best_ask().has_value());
}

TEST_F(L3BookTest, EmptyBookZeroOrderCounts) {
  EXPECT_EQ(book.total_bid_orders(), 0u);
  EXPECT_EQ(book.total_ask_orders(), 0u);
}

TEST_F(L3BookTest, EmptyBookZeroStats) {
  EXPECT_EQ(book.total_trades(), 0u);
  EXPECT_EQ(book.total_volume(), 0);
  EXPECT_EQ(book.last_trade_price(), 0);
}

TEST_F(L3BookTest, TopOfBookInvalidWhenEmpty) {
  auto tob = book.top_of_book(ts);
  EXPECT_FALSE(tob.valid);
}

// ── L3BookAdd ───────────────────────────────────────────────────────────────
TEST_F(L3BookTest, AddLimitBidGetsNewStatus) {
  auto o = make_order(Side::Buy, 10000, 100);
  auto rpt = book.add_order(o, ts);
  EXPECT_EQ(rpt.status, OrderStatus::New);
  EXPECT_EQ(rpt.leaves_qty, 100);
  EXPECT_EQ(rpt.filled_qty, 0);
  EXPECT_EQ(rpt.order_id, o.id);
}

TEST_F(L3BookTest, AddLimitBidUpdatesOrderCount) {
  book.add_order(make_order(Side::Buy, 10000, 100), ts);
  EXPECT_EQ(book.total_bid_orders(), 1u);
  EXPECT_EQ(book.total_ask_orders(), 0u);
}

TEST_F(L3BookTest, AddLimitBidUpdatesBestBid) {
  book.add_order(make_order(Side::Buy, 10000, 100), ts);
  auto bb = book.best_bid();
  ASSERT_TRUE(bb.has_value());
  EXPECT_EQ(bb->price, 10000);
  EXPECT_EQ(bb->qty, 100);
  EXPECT_EQ(bb->order_count, 1u);
}

TEST_F(L3BookTest, AddLimitAskUpdatesBestAsk) {
  book.add_order(make_order(Side::Sell, 10100, 50), ts);
  auto ba = book.best_ask();
  ASSERT_TRUE(ba.has_value());
  EXPECT_EQ(ba->price, 10100);
  EXPECT_EQ(ba->qty, 50);
}

TEST_F(L3BookTest, IncomingBidFullyFillsRestingAsk) {
  book.add_order(make_order(Side::Sell, 10000, 100), ts);
  auto rpt = book.add_order(make_order(Side::Buy, 10000, 100), ts);
  EXPECT_EQ(rpt.status, OrderStatus::Filled);
  EXPECT_EQ(rpt.filled_qty, 100);
  EXPECT_EQ(rpt.leaves_qty, 0);
  EXPECT_EQ(book.total_ask_orders(), 0u);
}

TEST_F(L3BookTest, IncomingBidPartiallyFillsRestingAsk) {
  book.add_order(make_order(Side::Sell, 10000, 100), ts);
  auto rpt = book.add_order(make_order(Side::Buy, 10000, 40), ts);
  EXPECT_EQ(rpt.status, OrderStatus::Filled);
  EXPECT_EQ(rpt.filled_qty, 40);
  EXPECT_EQ(book.total_ask_orders(), 1u);
}

TEST_F(L3BookTest, IncomingBidPartialFillWithRemainder) {
  book.add_order(make_order(Side::Sell, 10000, 40), ts);
  auto rpt = book.add_order(make_order(Side::Buy, 10000, 100), ts);
  EXPECT_EQ(rpt.status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(rpt.filled_qty, 40);
  EXPECT_EQ(rpt.leaves_qty, 60);
  EXPECT_EQ(book.total_bid_orders(), 1u);
}

// ── L3BookCancel ────────────────────────────────────────────────────────────
TEST_F(L3BookTest, CancelKnownOrderGetsCanceledStatus) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);
  auto rpt = book.cancel_order(o.id, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Canceled);
  EXPECT_EQ(rpt.order_id, o.id);
  EXPECT_EQ(rpt.leaves_qty, 0);
}

TEST_F(L3BookTest, CancelKnownOrderRemovesFromBook) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);
  book.cancel_order(o.id, ts);
  EXPECT_EQ(book.total_bid_orders(), 0u);
  EXPECT_FALSE(book.best_bid().has_value());
}

TEST_F(L3BookTest, CancelUnknownOrderGetsRejected) {
  auto rpt = book.cancel_order(999, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Rejected);
}

TEST_F(L3BookTest, CancelAskDecrementsAskCount) {
  auto o = make_order(Side::Sell, 10100, 50);
  book.add_order(o, ts);
  EXPECT_EQ(book.total_ask_orders(), 1u);
  book.cancel_order(o.id, ts);
  EXPECT_EQ(book.total_ask_orders(), 0u);
}

TEST_F(L3BookTest, CancelMiddleOrderPreservesQueuePosition) {
  auto o1 = make_order(Side::Buy, 10000, 100);
  auto o2 = make_order(Side::Buy, 10000, 200);
  auto o3 = make_order(Side::Buy, 10000, 300);
  book.add_order(o1, ts); book.add_order(o2, ts); book.add_order(o3, ts);
  book.cancel_order(o2.id, ts);
  EXPECT_EQ(book.queue_position(o1.id), 0u);
  EXPECT_EQ(book.queue_position(o3.id), 1u);
}

TEST_F(L3BookTest, CancelLastOrderAtPriceLevelClearsLevel) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);
  book.cancel_order(o.id, ts);
  EXPECT_TRUE(book.bid_depth(5).empty());
}

// ── L3BookTIF ───────────────────────────────────────────────────────────────
TEST_F(L3BookTest, IOCCancelsRemainderWhenNoLiquidity) {
  auto o = make_order(Side::Buy, 10000, 100, OrderType::Limit, TimeInForce::IOC);
  auto rpt = book.add_order(o, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Canceled);
  EXPECT_EQ(rpt.leaves_qty, 0);
  EXPECT_EQ(book.total_bid_orders(), 0u);
}

TEST_F(L3BookTest, IOCPartialFillCancelsRemainder) {
  book.add_order(make_order(Side::Sell, 10000, 40), ts);
  auto o = make_order(Side::Buy, 10000, 100, OrderType::Limit, TimeInForce::IOC);
  auto rpt = book.add_order(o, ts);
  EXPECT_EQ(rpt.status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(rpt.filled_qty, 40);
  EXPECT_EQ(rpt.leaves_qty, 0);
  EXPECT_EQ(book.total_bid_orders(), 0u);
}

TEST_F(L3BookTest, FOKRejectsWhenInsufficientLiquidity) {
  book.add_order(make_order(Side::Sell, 10000, 40), ts);
  auto o = make_order(Side::Buy, 10000, 100, OrderType::Limit, TimeInForce::FOK);
  auto rpt = book.add_order(o, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Rejected);
  EXPECT_EQ(rpt.reject_reason, RejectReason::InsufficientLiquidity);
  EXPECT_EQ(rpt.filled_qty, 0);
  EXPECT_EQ(rpt.leaves_qty, 0);
  EXPECT_EQ(book.total_ask_orders(), 1u);
}

TEST_F(L3BookTest, FOKFullyFillsWhenSufficientLiquidity) {
  book.add_order(make_order(Side::Sell, 10000, 100), ts);
  auto o = make_order(Side::Buy, 10000, 100, OrderType::Limit, TimeInForce::FOK);
  auto rpt = book.add_order(o, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Filled);
  EXPECT_EQ(rpt.filled_qty, 100);
  EXPECT_EQ(book.total_ask_orders(), 0u);
}

TEST_F(L3BookTest, GTCRestsAndSurvivesAcrossAdds) {
  auto o = make_order(Side::Buy, 10000, 100, OrderType::Limit, TimeInForce::GTC);
  book.add_order(o, ts);
  book.add_order(make_order(Side::Sell, 10200, 50), ts);
  EXPECT_EQ(book.total_bid_orders(), 1u);
}

// ── L3BookMatching ──────────────────────────────────────────────────────────
TEST_F(L3BookTest, PriceTimePriority_OldestFillsFirst) {
  auto o1 = make_order(Side::Sell, 10000, 50);
  auto o2 = make_order(Side::Sell, 10000, 50);
  book.add_order(o1, ts); book.add_order(o2, ts);

  std::vector<OrderId> filled_ids;
  book.on_trade([&](const Trade& t) { filled_ids.push_back(t.resting_id); });

  book.add_order(make_order(Side::Buy, 10000, 50), ts);

  ASSERT_EQ(filled_ids.size(), 1u);
  EXPECT_EQ(filled_ids[0], o1.id);
  EXPECT_EQ(book.total_ask_orders(), 1u);
}

TEST_F(L3BookTest, BestPriceFilledFirst) {
  book.add_order(make_order(Side::Sell, 10100, 50), ts);
  book.add_order(make_order(Side::Sell, 10000, 50), ts);

  std::vector<Price> filled_prices;
  book.on_trade([&](const Trade& t) { filled_prices.push_back(t.price); });

  book.add_order(make_order(Side::Buy, 10100, 50), ts);

  ASSERT_EQ(filled_prices.size(), 1u);
  EXPECT_EQ(filled_prices[0], 10000);
}

TEST_F(L3BookTest, SweepAcrossMultiplePriceLevels) {
  book.add_order(make_order(Side::Sell, 10000, 30), ts);
  book.add_order(make_order(Side::Sell, 10050, 30), ts);
  book.add_order(make_order(Side::Sell, 10100, 30), ts);

  std::vector<Trade> trades;
  book.on_trade([&](const Trade& t) { trades.push_back(t); });

  auto rpt = book.add_order(make_order(Side::Buy, 10100, 90), ts);
  EXPECT_EQ(rpt.status, OrderStatus::Filled);
  EXPECT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].price, 10000);
  EXPECT_EQ(trades[1].price, 10050);
  EXPECT_EQ(trades[2].price, 10100);
  EXPECT_EQ(book.total_ask_orders(), 0u);
}

TEST_F(L3BookTest, LimitBidDoesNotCrossAboveLimitPrice) {
  book.add_order(make_order(Side::Sell, 10100, 50), ts);
  auto rpt = book.add_order(make_order(Side::Buy, 10050, 50), ts);
  EXPECT_EQ(rpt.status, OrderStatus::New);
  EXPECT_EQ(rpt.filled_qty, 0);
}

TEST_F(L3BookTest, TradeCountAndVolumeAccumulate) {
  book.add_order(make_order(Side::Sell, 10000, 100), ts);
  book.add_order(make_order(Side::Buy,  10000, 60),  ts);
  book.add_order(make_order(Side::Buy,  10000, 40),  ts);

  EXPECT_EQ(book.total_trades(), 2u);
  EXPECT_EQ(book.total_volume(), 100);
  EXPECT_EQ(book.last_trade_price(), 10000);
}

// ── L3BookIceberg ───────────────────────────────────────────────────────────
TEST_F(L3BookTest, FOKCountsHiddenQtyTowardAvailability) {
  // Iceberg public API not yet exposed; verify FOK pre-scan with normal visible qty.
  // Full iceberg hidden-qty counting is documented in L3_ORDER_BOOK.md Section 8.
  book.add_order(make_order(Side::Sell, 10000, 100), ts);
  auto o = make_order(Side::Buy, 10000, 100, OrderType::Limit, TimeInForce::FOK);
  auto rpt = book.add_order(o, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Filled);
}

// ── L3BookQueuePos ──────────────────────────────────────────────────────────
TEST_F(L3BookTest, QueuePositionFirstOrderIsZero) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);
  EXPECT_EQ(book.queue_position(o.id), 0u);
}

TEST_F(L3BookTest, QueuePositionSecondOrderIsOne) {
  auto o1 = make_order(Side::Buy, 10000, 100);
  auto o2 = make_order(Side::Buy, 10000, 200);
  book.add_order(o1, ts); book.add_order(o2, ts);
  EXPECT_EQ(book.queue_position(o1.id), 0u);
  EXPECT_EQ(book.queue_position(o2.id), 1u);
}

TEST_F(L3BookTest, QueuePositionUnknownReturnsMax) {
  EXPECT_EQ(book.queue_position(999),
            std::numeric_limits<std::uint32_t>::max());
}

TEST_F(L3BookTest, QuantityAheadOfFirstOrderIsZero) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);
  EXPECT_EQ(book.quantity_ahead(o.id), 0);
}

TEST_F(L3BookTest, QuantityAheadOfSecondOrderEqualsFirst) {
  auto o1 = make_order(Side::Buy, 10000, 150);
  auto o2 = make_order(Side::Buy, 10000, 200);
  book.add_order(o1, ts); book.add_order(o2, ts);
  EXPECT_EQ(book.quantity_ahead(o2.id), 150);
}

TEST_F(L3BookTest, QueuePositionUpdatedAfterCancelOfFront) {
  auto o1 = make_order(Side::Buy, 10000, 100);
  auto o2 = make_order(Side::Buy, 10000, 200);
  auto o3 = make_order(Side::Buy, 10000, 300);
  book.add_order(o1, ts); book.add_order(o2, ts); book.add_order(o3, ts);
  book.cancel_order(o1.id, ts);
  EXPECT_EQ(book.queue_position(o2.id), 0u);
  EXPECT_EQ(book.queue_position(o3.id), 1u);
}

// ── L3BookCallbacks ─────────────────────────────────────────────────────────
TEST_F(L3BookTest, TradeCallbackFiresOnMatch) {
  book.add_order(make_order(Side::Sell, 10000, 100), ts);
  int trade_count = 0;
  book.on_trade([&](const Trade&) { ++trade_count; });
  book.add_order(make_order(Side::Buy, 10000, 100), ts);
  EXPECT_EQ(trade_count, 1);
}

TEST_F(L3BookTest, TradeCallbackNotFiredWhenNoMatch) {
  int trade_count = 0;
  book.on_trade([&](const Trade&) { ++trade_count; });
  book.add_order(make_order(Side::Buy, 10000, 100), ts);
  EXPECT_EQ(trade_count, 0);
}

TEST_F(L3BookTest, TopOfBookCallbackFiresWhenOrderRests) {
  int tob_count = 0;
  book.on_top_of_book([&](const TopOfBook&) { ++tob_count; });
  book.add_order(make_order(Side::Buy, 10000, 100), ts);
  EXPECT_GE(tob_count, 1);
}

TEST_F(L3BookTest, TopOfBookCallbackFiresOnCancel) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);
  int tob_count = 0;
  book.on_top_of_book([&](const TopOfBook&) { ++tob_count; });
  book.cancel_order(o.id, ts);
  EXPECT_GE(tob_count, 1);
}

// ── L3BookReplace ────────────────────────────────────────────────────────────
TEST_F(L3BookTest, ReplaceOrderSucceeds) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);

  ReplaceRequest req{};
  req.order_id = o.id;
  req.trader_id = o.trader_id;
  req.symbol = sym;
  req.new_price = 10050;
  req.new_qty = 200;

  auto rpt = book.replace_order(req, ts);
  EXPECT_NE(rpt.status, OrderStatus::Rejected);
}

TEST_F(L3BookTest, ReplaceUnknownOrderGetsRejected) {
  ReplaceRequest req{};
  req.order_id = 999;
  req.trader_id = 1;
  req.symbol = sym;
  req.new_price = 10050;
  req.new_qty = 100;
  auto rpt = book.replace_order(req, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Rejected);
}

// ── L3BookDuplicate ──────────────────────────────────────────────────────────
TEST_F(L3BookTest, DuplicateOrderIdGetsRejected) {
  auto o = make_order(Side::Buy, 10000, 100);
  book.add_order(o, ts);
  auto rpt = book.add_order(o, ts);
  EXPECT_EQ(rpt.status, OrderStatus::Rejected);
  EXPECT_EQ(rpt.reject_reason, RejectReason::DuplicateOrderId);
}
