#ifndef CAMEO_H
#define CAMEO_H

#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <vector>

#include "ChampSim/champsim_constants.h"
#include "ChampSim/channel.h"
#include "ChampSim/util/bits.h"
#include "ProjectConfiguration.h" // User file
#include "Ramulator/Request.h"

/** @note Abbreviation:
 *  FM -> Fast memory (e.g., HBM, DDR4)
 *  SM -> Slow memory (e.g., DDR4, PCM)
*/

#if (MEMORY_USE_OS_TRANSPARENT_MANAGEMENT == ENABLE)

#if (IDEAL_LINE_LOCATION_TABLE == ENABLE) || (COLOCATED_LINE_LOCATION_TABLE == ENABLE)
#define COUNTER_WIDTH                         uint8_t
#define COUNTER_MAX_VALUE                     (UINT8_MAX)
#define COUNTER_DEFAULT_VALUE                 (0)

#define HOTNESS_WIDTH                         bool
#define HOTNESS_DEFAULT_VALUE                 (false)

#define REMAPPING_LOCATION_WIDTH              uint8_t
#define REMAPPING_LOCATION_WIDTH_BITS         (3) // Default: 3
#define LOCATION_TABLE_ENTRY_WIDTH            uint16_t

#define NUMBER_OF_BLOCK                       (5) // Default: 5

// 0x0538 for the congruence group with 5 members (lines) at most, (000_001_010_011_100_0 = 0x0538)
// [15:13] bit for member 0, [12:10] bit for member 1, [9:7] bit for member 2, [6:4] bit for member 3, [3:1] bit for member 4.
#define LOCATION_TABLE_ENTRY_DEFAULT_VALUE    (0x0538)
#define LOCATION_TABLE_ENTRY_MSB              (UINT16_WIDTH - 1) // MSB -> most significant bit

#define REMAPPING_REQUEST_QUEUE_LENGTH        (64) // 1024/4096
#define QUEUE_BUSY_DEGREE_THRESHOLD           (0.8f)

#define INCOMPLETE_READ_REQUEST_QUEUE_LENGTH  (128)
#define INCOMPLETE_WRITE_REQUEST_QUEUE_LENGTH (128)

#if (BITS_MANIPULATION == DISABLE)
#undef REMAPPING_LOCATION_WIDTH_BITS
#undef NUMBER_OF_BLOCK
#define REMAPPING_LOCATION_WIDTH_BITS (champsim::lg2(64))
#define NUMBER_OF_BLOCK               (35)
#endif // BITS_MANIPULATION

class OS_TRANSPARENT_MANAGEMENT
{
    using channel_type = champsim::channel;
    using request_type = typename channel_type::request_type;

public:
    uint64_t cycle                  = 0;
    COUNTER_WIDTH hotness_threshold = 0;
    uint64_t total_capacity;       // Uint is byte
    uint64_t fast_memory_capacity; // Uint is byte
    uint64_t total_capacity_at_data_block_granularity;
    uint64_t fast_memory_capacity_at_data_block_granularity;
    uint8_t fast_memory_offset_bit; // Address format in the data management granularity

    std::vector<COUNTER_WIDTH>& counter_table; // A counter for every data block
    std::vector<HOTNESS_WIDTH>& hotness_table; // A hotness bit for every data block, true -> data block is hot, false -> data block is cold.

    /* Remapping request */
    struct RemappingRequest
    {
        uint64_t address_in_fm, address_in_sm; // Hardware address in fast and slow memories
        REMAPPING_LOCATION_WIDTH fm_location, sm_location;
        uint8_t size; // Number of cache lines to remap
    };

    std::deque<RemappingRequest> remapping_request_queue;
    uint64_t remapping_request_queue_congestion;

    // Scoped enumerations
    enum class RemappingLocation : REMAPPING_LOCATION_WIDTH
    {
        Zero = 0,
        One,
        Two,
        Three,
        Four,
        Max = NUMBER_OF_BLOCK
    };

    uint8_t congruence_group_msb; // Most significant bit of congruence group, and its address format is in the byte granularity

    /* Remapping table */
#if (BITS_MANIPULATION == ENABLE)
    std::vector<LOCATION_TABLE_ENTRY_WIDTH>& line_location_table; // Paper CAMEO: SRAM-Based LLT / Embed LLT in Stacked DRAM
#else
    struct LocationTableEntry
    {
        REMAPPING_LOCATION_WIDTH location[NUMBER_OF_BLOCK]; // Location field for each line, location[0] is for the line in NM

        LocationTableEntry()
        {
            for (REMAPPING_LOCATION_WIDTH i = 0; i < REMAPPING_LOCATION_WIDTH(RemappingLocation::Max); i++)
            {
                location[i] = i;
            }
        };
    };

    std::vector<LocationTableEntry>& line_location_table; // Paper CAMEO: SRAM-Based LLT / Embed LLT in Stacked DRAM
#endif // BITS_MANIPULATION

#if (COLOCATED_LINE_LOCATION_TABLE == ENABLE)
    /** @brief
     *  If a memory read request is mapped in slow memory, the memory controller needs first access the fast memory
     *  to get the Location Entry and Data (LEAD), and then access the slow memory based on that LEAD.
     */
    struct ReadRequest
    {
        request_type packet;
        bool fm_access_finish = false; // Whether the access in fast memory is completed.
    };

    std::vector<ReadRequest> incomplete_read_request_queue;

    /** @brief
     *  If a memory write request is received, the memory controller needs first to figure out where is the right
     *  place to write. So, the memory controller first access the fast memory to get the Location Entry and Data (LEAD),
     *  and then write the memory (fast or slow memory).
     */
    struct WriteRequest
    {
        request_type packet;
        bool fm_access_finish = false; // Whether the access in fast memory is completed.
    };

    std::vector<WriteRequest> incomplete_write_request_queue;
#endif // COLOCATED_LINE_LOCATION_TABLE

    /* Member functions */
    OS_TRANSPARENT_MANAGEMENT(uint64_t max_address, uint64_t fast_memory_max_address);
    ~OS_TRANSPARENT_MANAGEMENT();

    // Address is physical address and at byte granularity
#if (TRACKING_LOAD_STORE_STATISTICS == ENABLE)
    // Address is physical address and at byte granularity
    bool memory_activity_tracking(uint64_t address, ramulator::Request::Type type, access_type type_origin, float queue_busy_degree);
#else
    // Address is physical address and at byte granularity
    bool memory_activity_tracking(uint64_t address, ramulator::Request::Type type, float queue_busy_degree);
#endif // TRACKING_LOAD_STORE_STATISTICS

    // Translate the physical address to hardware address
    void physical_to_hardware_address(request_type& packet);
    void physical_to_hardware_address(uint64_t& address);

    bool issue_remapping_request(RemappingRequest& remapping_request);
    bool finish_remapping_request();

    // Detect cold data block
    void cold_data_detection();

#if (COLOCATED_LINE_LOCATION_TABLE == ENABLE)
    bool finish_fm_access_in_incomplete_read_request_queue(uint64_t h_address);
    bool finish_fm_access_in_incomplete_write_request_queue(uint64_t h_address);
#endif // COLOCATED_LINE_LOCATION_TABLE

private:
    // Evict cold data block
    bool cold_data_eviction(uint64_t source_address, float queue_busy_degree);

    // Add new remapping request into the remapping_request_queue
    bool enqueue_remapping_request(RemappingRequest& remapping_request);
};

#endif // IDEAL_LINE_LOCATION_TABLE, COLOCATED_LINE_LOCATION_TABLE

#endif // MEMORY_USE_OS_TRANSPARENT_MANAGEMENT
#endif // CAMEO_H
