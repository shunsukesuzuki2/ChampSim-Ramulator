#include "os_transparent_management.h"

#if (MEMORY_USE_OS_TRANSPARENT_MANAGEMENT == ENABLE)

#if (IDEAL_LINE_LOCATION_TABLE == ENABLE) || (COLOCATED_LINE_LOCATION_TABLE == ENABLE)
OS_TRANSPARENT_MANAGEMENT::OS_TRANSPARENT_MANAGEMENT(uint64_t max_address, uint64_t fast_memory_max_address)
: total_capacity(max_address), fast_memory_capacity(fast_memory_max_address),
  total_capacity_at_data_block_granularity(max_address >> DATA_MANAGEMENT_OFFSET_BITS),
  fast_memory_capacity_at_data_block_granularity(fast_memory_max_address >> DATA_MANAGEMENT_OFFSET_BITS),
  fast_memory_offset_bit(champsim::lg2(fast_memory_max_address)), // Note here only support integers of 2's power.
  counter_table(*(new std::vector<COUNTER_WIDTH>(max_address >> DATA_MANAGEMENT_OFFSET_BITS, COUNTER_DEFAULT_VALUE))),
  hotness_table(*(new std::vector<HOTNESS_WIDTH>(max_address >> DATA_MANAGEMENT_OFFSET_BITS, HOTNESS_DEFAULT_VALUE))),
  congruence_group_msb(REMAPPING_LOCATION_WIDTH_BITS + fast_memory_offset_bit - 1),
#if (BITS_MANIPULATION == ENABLE)
  line_location_table(*(new std::vector<LOCATION_TABLE_ENTRY_WIDTH>(fast_memory_max_address >> DATA_MANAGEMENT_OFFSET_BITS, LOCATION_TABLE_ENTRY_DEFAULT_VALUE)))
#else
  line_location_table(*(new std::vector<LocationTableEntry>(fast_memory_max_address >> DATA_MANAGEMENT_OFFSET_BITS)))
#endif // BITS_MANIPULATION
{
    hotness_threshold                            = HOTNESS_THRESHOLD;
    remapping_request_queue_congestion           = 0;

    uint64_t expected_number_in_congruence_group = total_capacity / fast_memory_capacity;
    if (expected_number_in_congruence_group > REMAPPING_LOCATION_WIDTH(RemappingLocation::Max))
    {
        std::cout << __func__ << ": congruence group error." << std::endl;
    }
    else
    {
        std::printf("Number in Congruence group: %ld.\n", expected_number_in_congruence_group);
    }
};

OS_TRANSPARENT_MANAGEMENT::~OS_TRANSPARENT_MANAGEMENT()
{
    output_statistics.remapping_request_queue_congestion = remapping_request_queue_congestion;

    delete &counter_table;
    delete &hotness_table;
    delete &line_location_table;
};

#if (TRACKING_LOAD_STORE_STATISTICS == ENABLE)
bool OS_TRANSPARENT_MANAGEMENT::memory_activity_tracking(uint64_t address, ramulator::Request::Type type, access_type type_origin, float queue_busy_degree)
{
#if (TRACKING_LOAD_ONLY)
    if (type_origin == access_type::RFO || type_origin == access_type::WRITE) // CPU Store Instruction and LLC Writeback is ignored
    {
        return true;
    }
#endif // TRACKING_LOAD_ONLY

#if (TRACKING_READ_ONLY)
    if (type == ramulator::Request::Type::WRITE) // Memory Write is ignored
    {
        return true;
    }
#endif // TRACKING_READ_ONLY

    if (address >= total_capacity)
    {
        std::cout << __func__ << ": address input error." << std::endl;
        return false;
    }

    uint64_t data_block_address        = address >> DATA_MANAGEMENT_OFFSET_BITS;                                                                     // Calculate the data block address
    uint64_t line_location_table_index = data_block_address % fast_memory_capacity_at_data_block_granularity;                                        // Calculate the index in line location table
    REMAPPING_LOCATION_WIDTH location  = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity); // Calculate the location in the entry of the line location table

    if (location >= REMAPPING_LOCATION_WIDTH(RemappingLocation::Max))
    {
        std::cout << __func__ << ": address input error (location)." << std::endl;
        abort();
    }

#if (BITS_MANIPULATION == ENABLE)
    uint8_t msb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * location;
    uint8_t lsb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * location - 1);

    REMAPPING_LOCATION_WIDTH remapping_location = champsim::get_bits(line_location_table.at(line_location_table_index), msb_in_location_table_entry, lsb_in_location_table_entry);
#else
    REMAPPING_LOCATION_WIDTH remapping_location = line_location_table.at(line_location_table_index).location[location];
#endif // BITS_MANIPULATION

    if (type == ramulator::Request::Type::READ) // For read request
    {
        if (counter_table.at(data_block_address) < COUNTER_MAX_VALUE)
        {
            counter_table[data_block_address]++; // Increment its counter
        }

        if (counter_table.at(data_block_address) >= hotness_threshold)
        {
            hotness_table.at(data_block_address) = true; // Mark hot data block
        }
    }
    else if (type == ramulator::Request::Type::WRITE) // For write request
    {
        if (counter_table.at(data_block_address) < COUNTER_MAX_VALUE)
        {
            counter_table[data_block_address]++; // Increment its counter
        }

        if (counter_table.at(data_block_address) >= hotness_threshold)
        {
            hotness_table.at(data_block_address) = true; // Mark hot data block
        }
    }
    else
    {
        std::cout << __func__ << ": type input error." << std::endl;
        assert(false);
    }

    // Add new remapping requests to queue
    if ((hotness_table.at(data_block_address) == true) && (remapping_location != REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)))
    {
        RemappingRequest remapping_request;
        REMAPPING_LOCATION_WIDTH fm_location = REMAPPING_LOCATION_WIDTH(RemappingLocation::Max);

#if (BITS_MANIPULATION == ENABLE)
        uint8_t fm_msb_in_location_table_entry;
        uint8_t fm_lsb_in_location_table_entry;
#endif // BITS_MANIPULATION

        REMAPPING_LOCATION_WIDTH fm_remapping_location;

        // Find the fm_location in the entry of line_location_table (where RemappingLocation::Zero is in the entry of line_location_table)
        for (REMAPPING_LOCATION_WIDTH i = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero); i < REMAPPING_LOCATION_WIDTH(RemappingLocation::Max); i++)
        {
#if (BITS_MANIPULATION == ENABLE)
            fm_msb_in_location_table_entry = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * i;
            fm_lsb_in_location_table_entry = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * i - 1);

            fm_remapping_location          = champsim::get_bits(line_location_table.at(line_location_table_index), fm_msb_in_location_table_entry, fm_lsb_in_location_table_entry);
#else
            fm_remapping_location = line_location_table.at(line_location_table_index).location[i];
#endif // BITS_MANIPULATION

            if (fm_remapping_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero))
            {
                // Found the location of fm_remapping_location in the entry of line_location_table
                fm_location = i;
                break;
            }
        }

        if (fm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Max))
        {
            std::cout << __func__ << ": find the fm_location error." << std::endl;
            abort();
        }

        if (fm_remapping_location == remapping_location) // Check
        {
            std::cout << __func__ << ": add new remapping request error 1." << std::endl;
#if (BITS_MANIPULATION == ENABLE)
            std::printf("line_location_table.at(%ld): %d.\n", line_location_table_index, line_location_table.at(line_location_table_index));
            std::printf("remapping_location: %d, fm_remapping_location: %d.\n", remapping_location, fm_remapping_location);
            std::printf("fm_msb_in_location_table_entry: %d, fm_lsb_in_location_table_entry: %d.\n", fm_msb_in_location_table_entry, fm_lsb_in_location_table_entry);
            std::printf("msb_in_location_table_entry: %d, lsb_in_location_table_entry: %d.\n", msb_in_location_table_entry, lsb_in_location_table_entry);
#else
            std::printf("line_location_table.at(%ld)\n", line_location_table_index);
            std::printf("remapping_location: %d, fm_remapping_location: %d.\n", remapping_location, fm_remapping_location);
#endif // BITS_MANIPULATION
            abort();
        }

        remapping_request.address_in_fm = champsim::replace_bits(line_location_table_index << DATA_MANAGEMENT_OFFSET_BITS, uint64_t(fm_remapping_location) << fast_memory_offset_bit, congruence_group_msb, fast_memory_offset_bit);
        remapping_request.address_in_sm = champsim::replace_bits(line_location_table_index << DATA_MANAGEMENT_OFFSET_BITS, uint64_t(remapping_location) << fast_memory_offset_bit, congruence_group_msb, fast_memory_offset_bit);

        // Indicate the positions in line location table entry for address_in_fm and address_in_sm.
        remapping_request.fm_location   = fm_location;
        remapping_request.sm_location   = location;

        remapping_request.size          = DATA_GRANULARITY_IN_CACHE_LINE;

        if (queue_busy_degree <= QUEUE_BUSY_DEGREE_THRESHOLD)
        {
            enqueue_remapping_request(remapping_request);
        }
    }

    return true;
};

#else
bool OS_TRANSPARENT_MANAGEMENT::memory_activity_tracking(uint64_t address, ramulator::Request::Type type, float queue_busy_degree)
{
    if (address >= total_capacity)
    {
        std::cout << __func__ << ": address input error." << std::endl;
        return false;
    }

    uint64_t data_block_address        = address >> DATA_MANAGEMENT_OFFSET_BITS;                                                                     // Calculate the data block address
    uint64_t line_location_table_index = data_block_address % fast_memory_capacity_at_data_block_granularity;                                        // Calculate the index in line location table
    REMAPPING_LOCATION_WIDTH location  = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity); // Calculate the location in the entry of the line location table

    if (location >= REMAPPING_LOCATION_WIDTH(RemappingLocation::Max))
    {
        std::cout << __func__ << ": address input error (location)." << std::endl;
        abort();
    }

#if (BITS_MANIPULATION == ENABLE)
    uint8_t msb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * location;
    uint8_t lsb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * location - 1);

    REMAPPING_LOCATION_WIDTH remapping_location = champsim::get_bits(line_location_table.at(line_location_table_index), msb_in_location_table_entry, lsb_in_location_table_entry);
#else
    REMAPPING_LOCATION_WIDTH remapping_location = line_location_table.at(line_location_table_index).location[location];
#endif // BITS_MANIPULATION

    if (type == ramulator::Request::Type::READ) // For read request
    {
        if (counter_table.at(data_block_address) < COUNTER_MAX_VALUE)
        {
            counter_table[data_block_address]++; // Increment its counter
        }

        if (counter_table.at(data_block_address) >= hotness_threshold)
        {
            hotness_table.at(data_block_address) = true; // Mark hot data block
        }
    }
    else if (type == ramulator::Request::Type::WRITE) // For write request
    {
        if (counter_table.at(data_block_address) < COUNTER_MAX_VALUE)
        {
            counter_table[data_block_address]++; // Increment its counter
        }

        if (counter_table.at(data_block_address) >= hotness_threshold)
        {
            hotness_table.at(data_block_address) = true; // Mark hot data block
        }
    }
    else
    {
        std::cout << __func__ << ": type input error." << std::endl;
        assert(false);
    }

    // Add new remapping requests to queue
    if ((hotness_table.at(data_block_address) == true) && (remapping_location != REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)))
    {
        RemappingRequest remapping_request;
        REMAPPING_LOCATION_WIDTH fm_location = REMAPPING_LOCATION_WIDTH(RemappingLocation::Max);

#if (BITS_MANIPULATION == ENABLE)
        uint8_t fm_msb_in_location_table_entry;
        uint8_t fm_lsb_in_location_table_entry;
#endif // BITS_MANIPULATION

        REMAPPING_LOCATION_WIDTH fm_remapping_location;

        // Find the fm_location in the entry of line_location_table (where RemappingLocation::Zero is in the entry of line_location_table)
        for (REMAPPING_LOCATION_WIDTH i = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero); i < REMAPPING_LOCATION_WIDTH(RemappingLocation::Max); i++)
        {
#if (BITS_MANIPULATION == ENABLE)
            fm_msb_in_location_table_entry = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * i;
            fm_lsb_in_location_table_entry = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * i - 1);

            fm_remapping_location          = champsim::get_bits(line_location_table.at(line_location_table_index), fm_msb_in_location_table_entry, fm_lsb_in_location_table_entry);
#else
            fm_remapping_location = line_location_table.at(line_location_table_index).location[i];
#endif // BITS_MANIPULATION

            if (fm_remapping_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero))
            {
                // Found the location of fm_remapping_location in the entry of line_location_table
                fm_location = i;
                break;
            }
        }

        if (fm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Max))
        {
            std::cout << __func__ << ": find the fm_location error." << std::endl;
            abort();
        }

        if (fm_remapping_location == remapping_location) // check
        {
            std::cout << __func__ << ": add new remapping request error 1." << std::endl;
#if (BITS_MANIPULATION == ENABLE)
            std::printf("line_location_table.at(%ld): %d.\n", line_location_table_index, line_location_table.at(line_location_table_index));
            std::printf("remapping_location: %d, fm_remapping_location: %d.\n", remapping_location, fm_remapping_location);
            std::printf("fm_msb_in_location_table_entry: %d, fm_lsb_in_location_table_entry: %d.\n", fm_msb_in_location_table_entry, fm_lsb_in_location_table_entry);
            std::printf("msb_in_location_table_entry: %d, lsb_in_location_table_entry: %d.\n", msb_in_location_table_entry, lsb_in_location_table_entry);
#else
            std::printf("line_location_table.at(%ld)\n", line_location_table_index);
            std::printf("remapping_location: %d, fm_remapping_location: %d.\n", remapping_location, fm_remapping_location);
#endif // BITS_MANIPULATION
            abort();
        }

        remapping_request.address_in_fm = champsim::replace_bits(line_location_table_index << DATA_MANAGEMENT_OFFSET_BITS, uint64_t(fm_remapping_location) << fast_memory_offset_bit, congruence_group_msb, fast_memory_offset_bit);
        remapping_request.address_in_sm = champsim::replace_bits(line_location_table_index << DATA_MANAGEMENT_OFFSET_BITS, uint64_t(remapping_location) << fast_memory_offset_bit, congruence_group_msb, fast_memory_offset_bit);

        // Indicate the positions in line location table entry for address_in_fm and address_in_sm.
        remapping_request.fm_location   = fm_location;
        remapping_request.sm_location   = location;

        remapping_request.size          = DATA_GRANULARITY_IN_CACHE_LINE;

        if (queue_busy_degree <= QUEUE_BUSY_DEGREE_THRESHOLD)
        {
            enqueue_remapping_request(remapping_request);
        }
    }

    return true;
};
#endif // TRACKING_LOAD_STORE_STATISTICS

void OS_TRANSPARENT_MANAGEMENT::physical_to_hardware_address(request_type& packet)
{
    uint64_t data_block_address        = packet.address >> DATA_MANAGEMENT_OFFSET_BITS;
    uint64_t line_location_table_index = data_block_address % fast_memory_capacity_at_data_block_granularity;
    REMAPPING_LOCATION_WIDTH location  = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity);

#if (BITS_MANIPULATION == ENABLE)
    uint8_t msb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * location;
    uint8_t lsb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * location - 1);

    REMAPPING_LOCATION_WIDTH remapping_location = champsim::get_bits(line_location_table.at(line_location_table_index), msb_in_location_table_entry, lsb_in_location_table_entry);
#else
    REMAPPING_LOCATION_WIDTH remapping_location = line_location_table.at(line_location_table_index).location[location];
#endif // BITS_MANIPULATION

    packet.h_address = champsim::replace_bits(champsim::replace_bits(line_location_table_index << DATA_MANAGEMENT_OFFSET_BITS, uint64_t(remapping_location) << fast_memory_offset_bit, congruence_group_msb, fast_memory_offset_bit), packet.address, DATA_MANAGEMENT_OFFSET_BITS - 1);

#if (COLOCATED_LINE_LOCATION_TABLE == ENABLE)
    packet.h_address_fm = champsim::replace_bits(champsim::replace_bits(line_location_table_index << DATA_MANAGEMENT_OFFSET_BITS, uint64_t(RemappingLocation::Zero) << fast_memory_offset_bit, congruence_group_msb, fast_memory_offset_bit), packet.address, DATA_MANAGEMENT_OFFSET_BITS - 1);
#endif // COLOCATED_LINE_LOCATION_TABLE
};

void OS_TRANSPARENT_MANAGEMENT::physical_to_hardware_address(uint64_t& address)
{
    uint64_t data_block_address        = address >> DATA_MANAGEMENT_OFFSET_BITS;
    uint64_t line_location_table_index = data_block_address % fast_memory_capacity_at_data_block_granularity;
    REMAPPING_LOCATION_WIDTH location  = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity);

#if (BITS_MANIPULATION == ENABLE)
    uint8_t msb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * location;
    uint8_t lsb_in_location_table_entry         = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * location - 1);

    REMAPPING_LOCATION_WIDTH remapping_location = champsim::get_bits(line_location_table.at(line_location_table_index), msb_in_location_table_entry, lsb_in_location_table_entry);
#else
    REMAPPING_LOCATION_WIDTH remapping_location = line_location_table.at(line_location_table_index).location[location];
#endif // BITS_MANIPULATION

    address = champsim::replace_bits(champsim::replace_bits(line_location_table_index << DATA_MANAGEMENT_OFFSET_BITS, uint64_t(remapping_location) << fast_memory_offset_bit, congruence_group_msb, fast_memory_offset_bit), address, DATA_MANAGEMENT_OFFSET_BITS - 1);
};

bool OS_TRANSPARENT_MANAGEMENT::issue_remapping_request(RemappingRequest& remapping_request)
{
    if (remapping_request_queue.empty() == false)
    {
        remapping_request = remapping_request_queue.front();
        return true;
    }

    return false;
};

bool OS_TRANSPARENT_MANAGEMENT::finish_remapping_request()
{
    if (remapping_request_queue.empty() == false)
    {
        RemappingRequest remapping_request = remapping_request_queue.front();
        remapping_request_queue.pop_front();

        uint64_t data_block_address        = remapping_request.address_in_fm >> DATA_MANAGEMENT_OFFSET_BITS;
        // data_block_address = remapping_request.address_in_sm >> DATA_MANAGEMENT_OFFSET_BITS;
        uint64_t line_location_table_index = data_block_address % fast_memory_capacity_at_data_block_granularity;

#if (BITS_MANIPULATION == ENABLE)
        uint8_t fm_msb_in_location_table_entry            = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * remapping_request.fm_location;
        uint8_t fm_lsb_in_location_table_entry            = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * remapping_request.fm_location - 1);
        uint8_t sm_msb_in_location_table_entry            = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * remapping_request.sm_location;
        uint8_t sm_lsb_in_location_table_entry            = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * remapping_request.sm_location - 1);

        REMAPPING_LOCATION_WIDTH fm_remapping_location    = champsim::get_bits(line_location_table.at(line_location_table_index), fm_msb_in_location_table_entry, fm_lsb_in_location_table_entry);
        REMAPPING_LOCATION_WIDTH sm_remapping_location    = champsim::get_bits(line_location_table.at(line_location_table_index), sm_msb_in_location_table_entry, sm_lsb_in_location_table_entry);

        line_location_table.at(line_location_table_index) = champsim::replace_bits(line_location_table.at(line_location_table_index), uint64_t(fm_remapping_location) << sm_lsb_in_location_table_entry, sm_msb_in_location_table_entry, sm_lsb_in_location_table_entry);
        line_location_table.at(line_location_table_index) = champsim::replace_bits(line_location_table.at(line_location_table_index), uint64_t(sm_remapping_location) << fm_lsb_in_location_table_entry, fm_msb_in_location_table_entry, fm_lsb_in_location_table_entry);
#else
        REMAPPING_LOCATION_WIDTH fm_remapping_location                                            = line_location_table.at(line_location_table_index).location[remapping_request.fm_location];
        REMAPPING_LOCATION_WIDTH sm_remapping_location                                            = line_location_table.at(line_location_table_index).location[remapping_request.sm_location];

        line_location_table.at(line_location_table_index).location[remapping_request.sm_location] = fm_remapping_location;
        line_location_table.at(line_location_table_index).location[remapping_request.fm_location] = sm_remapping_location;
#endif // BITS_MANIPULATION

        // Sanity check
        if (fm_remapping_location == sm_remapping_location)
        {
            std::cout << __func__ << ": read remapping location error." << std::endl;
            abort();
        }

        REMAPPING_LOCATION_WIDTH sum_of_remapping_location = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero);
        for (REMAPPING_LOCATION_WIDTH i = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero); i < REMAPPING_LOCATION_WIDTH(RemappingLocation::Max); i++)
        {
#if (BITS_MANIPULATION == ENABLE)
            uint8_t msb_in_location_table_entry = LOCATION_TABLE_ENTRY_MSB - REMAPPING_LOCATION_WIDTH_BITS * i;
            uint8_t lsb_in_location_table_entry = LOCATION_TABLE_ENTRY_MSB - (REMAPPING_LOCATION_WIDTH_BITS + REMAPPING_LOCATION_WIDTH_BITS * i - 1);

            sum_of_remapping_location += champsim::get_bits(line_location_table.at(line_location_table_index), msb_in_location_table_entry, lsb_in_location_table_entry);
#else
            sum_of_remapping_location += line_location_table.at(line_location_table_index).location[i];
#endif // BITS_MANIPULATION
        }

        REMAPPING_LOCATION_WIDTH correct_result = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero);
        for (REMAPPING_LOCATION_WIDTH i = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero); i < REMAPPING_LOCATION_WIDTH(RemappingLocation::Max); i++)
        {
            correct_result += i;
        }

        if (sum_of_remapping_location != correct_result)
        {
            std::cout << __func__ << ": sum_of_remapping_location verification error." << std::endl;
            std::printf("sum_of_remapping_location: %d, correct_result: %d.\n", sum_of_remapping_location, correct_result);
            abort();
        }
    }
    else
    {
        std::cout << __func__ << ": remapping error." << std::endl;
        assert(false);
        return false; // Error
    }

    return true;
};

void OS_TRANSPARENT_MANAGEMENT::cold_data_detection()
{
    cycle++;
}

bool OS_TRANSPARENT_MANAGEMENT::cold_data_eviction(uint64_t source_address, float queue_busy_degree)
{
    return false;
}

bool OS_TRANSPARENT_MANAGEMENT::enqueue_remapping_request(RemappingRequest& remapping_request)
{
    uint64_t data_block_address        = remapping_request.address_in_fm >> DATA_MANAGEMENT_OFFSET_BITS;
    uint64_t line_location_table_index = data_block_address % fast_memory_capacity_at_data_block_granularity;

    // Check duplicated remapping request in remapping_request_queue
    // If duplicated remapping requests exist, we won't add this new remapping request into the remapping_request_queue.
    bool duplicated_remapping_request  = false;
    for (uint64_t i = 0; i < remapping_request_queue.size(); i++)
    {
        uint64_t data_block_address_to_check        = remapping_request_queue[i].address_in_fm >> DATA_MANAGEMENT_OFFSET_BITS;
        uint64_t line_location_table_index_to_check = data_block_address_to_check % fast_memory_capacity_at_data_block_granularity;

        if (line_location_table_index_to_check == line_location_table_index)
        {
            duplicated_remapping_request = true; // Find a duplicated remapping request

            break;
        }
    }

    if (duplicated_remapping_request == false)
    {
        if (remapping_request_queue.size() < REMAPPING_REQUEST_QUEUE_LENGTH)
        {
            if (remapping_request.address_in_fm == remapping_request.address_in_sm) // Check
            {
                std::cout << __func__ << ": add new remapping request error 2." << std::endl;
                abort();
            }

            // Enqueue a remapping request
            remapping_request_queue.push_back(remapping_request);
        }
        else
        {
            // std::cout << __func__ << ": remapping_request_queue is full." << std::endl;
            remapping_request_queue_congestion++;
        }
    }
    else
    {
        return false;
    }

    // New remapping request is issued.
    return true;
}

#if (COLOCATED_LINE_LOCATION_TABLE == ENABLE)
bool OS_TRANSPARENT_MANAGEMENT::finish_fm_access_in_incomplete_read_request_queue(uint64_t h_address)
{
    for (size_t i = 0; i < incomplete_read_request_queue.size(); i++)
    {
        if (incomplete_read_request_queue[i].packet.h_address == h_address)
        {
            if (incomplete_read_request_queue[i].fm_access_finish == false)
            {
                incomplete_read_request_queue[i].fm_access_finish = true;
                return true;
            }
            else
            {
                continue;
            }
        }
    }

    return false;
}

bool OS_TRANSPARENT_MANAGEMENT::finish_fm_access_in_incomplete_write_request_queue(uint64_t h_address)
{
    for (size_t i = 0; i < incomplete_write_request_queue.size(); i++)
    {
        if (incomplete_write_request_queue[i].packet.h_address == h_address)
        {
            if (incomplete_write_request_queue[i].fm_access_finish == false)
            {
                incomplete_write_request_queue[i].fm_access_finish = true;
                return true;
            }
            else
            {
                continue;
            }
        }
    }

    return false;
}
#endif // COLOCATED_LINE_LOCATION_TABLE

#endif // IDEAL_LINE_LOCATION_TABLE, COLOCATED_LINE_LOCATION_TABLE
#endif // MEMORY_USE_OS_TRANSPARENT_MANAGEMENT
