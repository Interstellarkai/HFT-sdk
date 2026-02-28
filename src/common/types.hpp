#pragma once
// ============================================================================
// HFT-sdk — Core Types & Data Structures
// Fundamental type aliases, enumerations, and POD structures shared across
// all HFT-sdk modules: orders, trades, book levels, execution reports,
// market data snapshots, and inter-thread event wrappers.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <array>
#include <atomic>
#include <limits>
#include <string>
#include <algorithm>


namespace HFT_sdk {

using OrderId   = std::uint64_t;
using TraderId  = std::uint64_t;
using SymbolId  = std::string;
using Price     = std::int64_t;   // e.g. price in ticks
using Quantity  = std::int64_t;   // quantity in units
using Timestamp = std::uint64_t;    // nanoseconds since epoch

// ── Enumerations ───────────────────────────────────────────────────────────
enum class OrderStatus : std::uint8_t {
    New              = 0,
    PartiallyFilled  = 1,
    Filled           = 2,
    Canceled         = 3,
    Rejected         = 4,
    PendingCancel    = 5,
    PendingReplace   = 6
};

enum class RejectReason : std::uint8_t {
    None              = 0,
    UnknownSymbol     = 1,
    InvalidPrice      = 2,
    InvalidQuantity   = 3,
    PositionLimit     = 4,
    RateLimit         = 5,
    SelfTradePrevention = 6,
    KillSwitchActive  = 7,
    DuplicateOrderId  = 8,
    InsufficientLiquidity = 9
};

enum class Side : std::uint8_t {
    Buy  = 0,
    Sell = 1
};

enum class OrderType : std::uint8_t {
    Limit  = 0,
    Market = 1
};

enum class TimeInForce : std::uint8_t {
    Day = 0,
    IOC = 1,   // Immediate-or-Cancel
    FOK = 2,   // Fill-or-Kill
    GTC = 3    // Good-til-Cancel
};

// ── Fixed-size symbol representation (no heap allocation) ──────────────────
struct alignas(8) Symbol {
    static constexpr std::size_t MAX_LEN = 8;
    std::array<char, MAX_LEN> data{};

    Symbol() = default;

    explicit Symbol(const char* s) {
        std::size_t len = std::strlen(s);
        if (len > MAX_LEN) len = MAX_LEN;
        std::memcpy(data.data(), s, len);
    }

    explicit Symbol(const std::string& s) : Symbol(s.c_str()) {}

    bool operator==(const Symbol& o) const { return data == o.data; }
    bool operator!=(const Symbol& o) const { return data != o.data; }

    std::string to_string() const {
        auto it = std::find(data.begin(), data.end(), '\0');
        return std::string(data.begin(), it);
    }
};

struct SymbolHash {
    std::size_t operator()(const Symbol& s) const {
        std::uint64_t v;
        std::memcpy(&v, s.data.data(), sizeof(v));
        // FNV-1a style mix
        v ^= v >> 33;
        v *= 0xff51afd7ed558ccdULL;
        v ^= v >> 33;
        return static_cast<std::size_t>(v);
    }
};

// ── Fundamental numeric types ──────────────────────────────────────────────

static constexpr OrderId   INVALID_ORDER_ID  = 0;
static constexpr Price     INVALID_PRICE     = std::numeric_limits<Price>::max();
static constexpr Quantity  INVALID_QTY       = 0;

// ── Order Structures (cache-line aligned, no heap) ─────────────────────────
struct alignas(64) Order {
    OrderId     id          = INVALID_ORDER_ID;
    TraderId    trader_id   = 0;
    Symbol      symbol;
    Side        side        = Side::Buy;
    OrderType   type        = OrderType::Limit;
    TimeInForce tif         = TimeInForce::Day;
    Price       price       = 0;
    Quantity    quantity    = 0;
    Quantity    filled_qty  = 0;
    OrderStatus status      = OrderStatus::New;
    Timestamp   submit_time = 0;
    Timestamp   accept_time = 0;
    std::uint32_t queue_position = 0;   // position in price level queue
    char        cl_ord_id[20] = {};     // client order ID for FIX
};

struct CancelRequest {
    OrderId     order_id    = INVALID_ORDER_ID;
    TraderId    trader_id   = 0;
    Symbol      symbol;
    Timestamp   submit_time = 0;
    char        cl_ord_id[20] = {};
};

struct ReplaceRequest {
    OrderId     order_id    = INVALID_ORDER_ID;
    TraderId    trader_id   = 0;
    Symbol      symbol;
    Price       new_price   = 0;
    Quantity    new_qty     = 0;
    Timestamp   submit_time = 0;
    char        cl_ord_id[20] = {};
    char        orig_cl_ord_id[20] = {};
};

struct NewOrder {
    OrderId     id;
    TraderId    trader;
    SymbolId    symbol;
    Side        side;
    OrderType   type;
    TimeInForce tif;
    Price       price;      // ignored for market orders
    Quantity    qty;
};

struct CancelOrder {
    OrderId id;
};

struct alignas(64) Trade {
    std::uint64_t trade_id      = 0;
    OrderId       resting_id;
    OrderId       incoming_id;
    TraderId      resting_trader = 0;
    TraderId      incoming_trader = 0;
    Symbol        symbol;
    Side          aggressor_side;
    Price         price;
    Quantity      qty;
    Timestamp     match_timestamp_ns;
};

struct BookLevel {
    Price    price;
    Quantity qty;
    std::uint32_t order_count = 0;
};

struct TopOfBook {
    Symbol    symbol;
    BookLevel best_bid;
    BookLevel best_ask;
    Price     mid_price    = 0;
    Price     micro_price  = 0;
    Price     spread       = 0;
    Timestamp timestamp    = 0;
    bool     valid{false};
};

struct BookOrder {
    OrderId   id;
    TraderId  trader;
    Quantity  qty;
    Price     price;
    Side      side;
    OrderType type;  // Added for matching logic
    std::uint64_t ts_ns; // arrival time, for time priority

    BookOrder(const NewOrder& o, std::uint64_t ts_ns)
    {
        this->id     = o.id;
        this->trader = o.trader;
        this->qty    = o.qty;
        this->price  = o.price;
        this->side   = o.side;
        this->type   = o.type;
        this->ts_ns  = ts_ns;
    }
};
// ── Trade / Execution structures ───────────────────────────────────────────

struct ExecutionReport {
    OrderId     order_id    = INVALID_ORDER_ID;
    TraderId    trader_id   = 0;
    Symbol      symbol;
    Side        side        = Side::Buy;
    OrderType   type        = OrderType::Limit;
    OrderStatus status      = OrderStatus::New;
    Price       price       = 0;
    Quantity    order_qty   = 0;
    Quantity    filled_qty  = 0;
    Quantity    last_qty    = 0;
    Price       last_price  = 0;
    Quantity    leaves_qty  = 0;
    Timestamp   transact_time = 0;
    RejectReason reject_reason = RejectReason::None;
    char        cl_ord_id[20] = {};
    char        exec_id[20]   = {};
};

// ── Market Data structures ─────────────────────────────────────────────────

static constexpr std::size_t MAX_DEPTH_LEVELS = 20;

struct DepthSnapshot {
    Symbol    symbol;
    std::array<BookLevel, MAX_DEPTH_LEVELS> bids;
    std::array<BookLevel, MAX_DEPTH_LEVELS> asks;
    std::uint32_t bid_levels = 0;
    std::uint32_t ask_levels = 0;
    Timestamp timestamp      = 0;
};

// ── Internal event types for inter-thread communication ────────────────────
enum class EventType : std::uint8_t {
    NewOrder       = 0,
    CancelOrder    = 1,
    ReplaceOrder   = 2,
    ExecutionRpt   = 3,
    TradeEvent     = 4,
    L1Update       = 5,
    L2Update       = 6,
    L3Update       = 7,
    SimTick        = 8,
    Shutdown       = 255
};

struct alignas(64) EngineEvent {
    EventType type = EventType::NewOrder;
    union {
        Order          order;
        CancelRequest  cancel;
        ReplaceRequest replace;
        ExecutionReport exec_report;
        Trade           trade;
        TopOfBook       tob;
        DepthSnapshot   depth;
    };
    Timestamp event_time = 0;

    EngineEvent() : type(EventType::NewOrder), order{} {}
    ~EngineEvent() = default;
    EngineEvent(const EngineEvent& o) { std::memcpy(this, &o, sizeof(EngineEvent)); }
    EngineEvent& operator=(const EngineEvent& o) {
        if (this != &o) std::memcpy(this, &o, sizeof(EngineEvent));
        return *this;
    }
};

} // namespace HFT_sdk
