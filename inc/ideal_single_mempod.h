#ifndef IDEAL_SINGLE_MEMPOD_H
#define IDEAL_SINGLE_MEMPOD_H

#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <unordered_map>
#include <vector>

#include "ChampSim/champsim_constants.h"
#include "ChampSim/channel.h"
#include "ChampSim/util.h"
#include "ProjectConfiguration.h" // User file
#include "Ramulator/Request.h"

/** @note Abbreviation:
 *  FM -> Fast memory (e.g., HBM, DDR4)
 *  SM -> Slow memory (e.g., DDR4, PCM)
*/

/*
    MemPod's MEA Counter tracks data segment at DATA_MANAGEMENT_GRANULARITY, 
    and at Physical Address, not Hardware Address.
*/

#if (MEMORY_USE_OS_TRANSPARENT_MANAGEMENT == ENABLE)

#if (IDEAL_SINGLE_MEMPOD == ENABLE)

/* MemPod Parameter Setting */
#define TIME_INTERVAL_MEMPOD_us               (50)                                            // [us]
#define DATA_MANAGEMENT_GRANULARITY           (2048)                                          // [B] default:2048
#define DATA_MANAGEMENT_OFFSET_BITS           (champsim::lg2(DATA_MANAGEMENT_GRANULARITY))    // [bit]
#define CACHE_LINE_SIZE                       (64)                                            // [B]
#define SWAP_DATA_CACHE_LINES                 (DATA_MANAGEMENT_GRANULARITY / CACHE_LINE_SIZE) // [lines]

/* For mea_counter_table */
#define NUMBER_MEA_COUNTER                    (16u)
#define MEA_COUNTER_WIDTH                     uint8_t
#define MEA_COUNTER_MAX_VALUE                 (4u)
#define COUNTER_DEFAULT_VALUE                 (0)
#define MEA_COUNTER_RESET_EVERY_EPOCH         (DISABLE)

/* For address_remapping_table */
#define REMAPPING_TABLE_ENTRY_WIDTH           uint64_t

/* For swapping */
#define REMAPPING_REQUEST_QUEUE_LENGTH        (4096) // 1024/4096
#define QUEUE_BUSY_DEGREE_THRESHOLD_UP        (0.9f)
#define QUEUE_BUSY_DEGREE_THRESHOLD_DOWN      (0.8f)
#define QUEUE_BUSY_DEGREE_THRESHOLD           (0.8f)

#define INCOMPLETE_READ_REQUEST_QUEUE_LENGTH  (128)
#define INCOMPLETE_WRITE_REQUEST_QUEUE_LENGTH (128)

class OS_TRANSPARENT_MANAGEMENT
{
    using channel_type = champsim::channel;
    using request_type = typename channel_type::request_type;

public:
    uint64_t cycle = 0;
    uint64_t total_capacity;       // [B]
    uint64_t fast_memory_capacity; // [B]
    uint64_t total_capacity_at_granularity;
    uint64_t fast_memory_capacity_at_granularity;
    uint8_t fast_memory_offset_bit;                                          // Address format in the data management granularity
    uint8_t swap_size                               = SWAP_DATA_CACHE_LINES; // == 32
    REMAPPING_TABLE_ENTRY_WIDTH swap_fm_address_itr = 0;

    /* Remapping request */
    struct RemappingRequest
    {
        uint64_t h_address_in_fm, h_address_in_sm; // Hardware address in fast and slow memories
        uint64_t p_address_in_fm, p_address_in_sm; // Physical address in fast and slow memories
        uint8_t size;                              // Number of cache lines to remap == 32 (2048B)
    };

    struct PhysicalHardwareAddressTuple
    {
        uint64_t p_address, h_address;

        bool operator<(const PhysicalHardwareAddressTuple& right) const
        {
            return h_address == right.h_address ? p_address < right.p_address : h_address < right.h_address;
        }
    };

    std::unordered_map<REMAPPING_TABLE_ENTRY_WIDTH, MEA_COUNTER_WIDTH>& mea_counter_table;
    std::unordered_map<REMAPPING_TABLE_ENTRY_WIDTH, REMAPPING_TABLE_ENTRY_WIDTH>& address_remapping_table;
    std::unordered_map<REMAPPING_TABLE_ENTRY_WIDTH, REMAPPING_TABLE_ENTRY_WIDTH>& invert_address_remapping_table;

    std::deque<RemappingRequest> remapping_request_queue;
    uint64_t remapping_request_queue_congestion;

    double interval_cycle;
    double next_interval_cycle;
    uint32_t intervals;

    /* Member functions */
    OS_TRANSPARENT_MANAGEMENT(uint64_t max_address, uint64_t fast_memory_max_address);
    ~OS_TRANSPARENT_MANAGEMENT();

#if (TRACKING_LOAD_STORE_STATISTICS == ENABLE)
    // Address is physical address and at byte granularity
    bool memory_activity_tracking(uint64_t address, ramulator::Request::Type type, ramulator::Request::Type type_origin, float queue_busy_degree);
#else
    // Address is physical address and at byte granularity
    bool memory_activity_tracking(uint64_t address, ramulator::Request::Type type, float queue_busy_degree);
#endif // TRACKING_LOAD_STORE_STATISTICS

    // Translate the physical address to hardware address
    void physical_to_hardware_address(request_type& packet);
    void physical_to_hardware_address(uint64_t& address);

    // Detect cold data block and cycle increment
    void cold_data_detection();

    // MemPod interval swap
    void check_interval_swap(uint8_t swapping_states, bool warmup);
    bool issue_remapping_request(RemappingRequest& remapping_request);
    bool finish_remapping_request();

private:
    void get_hot_page_from_mea_counter(std::vector<REMAPPING_TABLE_ENTRY_WIDTH>& hot_pages);
    void determine_swap_pair(std::vector<REMAPPING_TABLE_ENTRY_WIDTH>& hot_pages, bool warmup);

    void update_mea_counter(uint64_t segment_address);

    void reset_mea_counter();
    void cancel_not_started_remapping_request(uint8_t swapping_states);
    bool enqueue_remapping_request(RemappingRequest& remapping_request, bool warmup);
};

#endif // IDEAL_SINGLE_MEMPOD

#endif // MEMORY_USE_OS_TRANSPARENT_MANAGEMENT
#endif // OS_TRANSPARENT_MANAGEMENT_H
