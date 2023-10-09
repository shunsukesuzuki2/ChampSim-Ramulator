#include "os_transparent_management.h"

#if (MEMORY_USE_OS_TRANSPARENT_MANAGEMENT == ENABLE)

#if (IDEAL_VARIABLE_GRANULARITY == ENABLE)
OS_TRANSPARENT_MANAGEMENT::OS_TRANSPARENT_MANAGEMENT(uint64_t max_address, uint64_t fast_memory_max_address)
: total_capacity(max_address), fast_memory_capacity(fast_memory_max_address),
  total_capacity_at_data_block_granularity(max_address >> DATA_MANAGEMENT_OFFSET_BITS),
  fast_memory_capacity_at_data_block_granularity(fast_memory_max_address >> DATA_MANAGEMENT_OFFSET_BITS),
  fast_memory_offset_bit(champsim::lg2(fast_memory_max_address)), // Note here only support integers of 2's power.
  counter_table(*(new std::vector<COUNTER_WIDTH>(max_address >> DATA_MANAGEMENT_OFFSET_BITS, COUNTER_DEFAULT_VALUE))),
  hotness_table(*(new std::vector<HOTNESS_WIDTH>(max_address >> DATA_MANAGEMENT_OFFSET_BITS, HOTNESS_DEFAULT_VALUE))),
  set_msb(REMAPPING_LOCATION_WIDTH_BITS + fast_memory_offset_bit - 1),
  access_table(*(new std::vector<AccessDistribution>(max_address >> DATA_MANAGEMENT_OFFSET_BITS))),
  placement_table(*(new std::vector<PlacementEntry>(fast_memory_max_address >> DATA_MANAGEMENT_OFFSET_BITS)))
{
    hotness_threshold                   = HOTNESS_THRESHOLD;
    remapping_request_queue_congestion  = 0;

    expected_number_in_congruence_group = total_capacity / fast_memory_capacity;
    std::printf("Number in Congruence group: %ld.\n", expected_number_in_congruence_group);
};

OS_TRANSPARENT_MANAGEMENT::~OS_TRANSPARENT_MANAGEMENT()
{
    output_statistics.remapping_request_queue_congestion = remapping_request_queue_congestion;

#if (STATISTICS_INFORMATION == ENABLE)
    uint64_t estimated_spatial_locality_counts[MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Max)] = {0};
    uint64_t estimated_spatial_locality_total_counts                                                   = 0;
    uint64_t granularity_counts[MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Max)]                = {0};
    uint64_t granularity_total_counts                                                                  = 0;
    uint64_t granularity_predict_counts[MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Max)]        = {0};
    uint64_t granularity_total_predict_counts                                                          = 0;
    uint64_t access_table_size                                                                         = total_capacity >> DATA_MANAGEMENT_OFFSET_BITS;

    for (uint64_t i = 0; i < access_table_size; i++)
    {
        if (access_table.at(i).access_flag == true)
        {
            // Calculate the start address
            START_ADDRESS_WIDTH start_address = START_ADDRESS_WIDTH(StartAddress::Zero);
            for (START_ADDRESS_WIDTH j = START_ADDRESS_WIDTH(StartAddress::Zero); j < START_ADDRESS_WIDTH(StartAddress::Max); j++)
            {
                if (access_table.at(i).access_stats[j] == true)
                {
                    start_address = j;
                    break;
                }
            }

            // Calculate the end address
            START_ADDRESS_WIDTH end_address = START_ADDRESS_WIDTH(StartAddress::Zero);
            for (START_ADDRESS_WIDTH j = START_ADDRESS_WIDTH(StartAddress::Max) - 1; j > START_ADDRESS_WIDTH(StartAddress::Zero); j--)
            {
                if (access_table.at(i).access_stats[j] == true)
                {
                    end_address = j;
                    break;
                }
            }

            estimated_spatial_locality_counts[access_table.at(i).estimated_spatial_locality_stats]++;

            access_table.at(i).granularity_stats = end_address - start_address + 1;
            granularity_counts[access_table.at(i).granularity_stats]++;

            granularity_predict_counts[access_table.at(i).granularity_predict_stats]++;
        }
    }

    fprintf(output_statistics.file_handler, "\n\nInformation about variable granularity\n\n");

    // Print out spatial locality
    fprintf(output_statistics.file_handler, "Estimated spatial locality distribution:\n");
    for (MIGRATION_GRANULARITY_WIDTH i = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None); i < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Max); i++)
    {
        fprintf(output_statistics.file_handler, "Spatial locality [%d] %ld\n", i, estimated_spatial_locality_counts[i]);
        estimated_spatial_locality_total_counts += estimated_spatial_locality_counts[i];
    }
    fprintf(output_statistics.file_handler, "estimated_spatial_locality_total_counts %ld\n", estimated_spatial_locality_total_counts);

    // Print out best granularity
    fprintf(output_statistics.file_handler, "\nBest granularity distribution:\n");
    for (MIGRATION_GRANULARITY_WIDTH i = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None); i < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Max); i++)
    {
        fprintf(output_statistics.file_handler, "Granularity [%d] %ld\n", i, granularity_counts[i]);
        granularity_total_counts += granularity_counts[i];
    }
    fprintf(output_statistics.file_handler, "granularity_total_counts %ld\n", granularity_total_counts);

    fprintf(output_statistics.file_handler, "\nPredicted granularity distribution:\n");
    for (MIGRATION_GRANULARITY_WIDTH i = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None); i < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Max); i++)
    {
        fprintf(output_statistics.file_handler, "Granularity [%d] %ld\n", i, granularity_predict_counts[i]);
        granularity_total_predict_counts += granularity_predict_counts[i];
    }
    fprintf(output_statistics.file_handler, "granularity_total_predict_counts %ld\n", granularity_total_predict_counts);

#endif // STATISTICS_INFORMATION

    delete &counter_table;
    delete &hotness_table;
    delete &access_table;
    delete &placement_table;
};

bool OS_TRANSPARENT_MANAGEMENT::memory_activity_tracking(uint64_t address, ramulator::Request::Type type, float queue_busy_degree)
{
    if (address >= total_capacity)
    {
        std::cout << __func__ << ": address input error." << std::endl;
        return false;
    }

    uint64_t data_block_address                                   = address >> DATA_MANAGEMENT_OFFSET_BITS;                              // Calculate the data block address
    uint64_t placement_table_index                                = data_block_address % fast_memory_capacity_at_data_block_granularity; // Calculate the index in placement table
    uint64_t base_remapping_address                               = placement_table_index << DATA_MANAGEMENT_OFFSET_BITS;
    REMAPPING_LOCATION_WIDTH tag                                  = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity); // Calculate the tag of the data block address
    uint64_t first_address                                        = data_block_address << DATA_MANAGEMENT_OFFSET_BITS;                                                          // The first address in the page granularity of this address
    START_ADDRESS_WIDTH data_line_positon                         = START_ADDRESS_WIDTH((address >> DATA_LINE_OFFSET_BITS) - (first_address >> DATA_LINE_OFFSET_BITS));

    // Mark accessed data line
    access_table.at(data_block_address).access[data_line_positon] = true;

#if (STATISTICS_INFORMATION == ENABLE)
    access_table.at(data_block_address).access_flag                              = true;
    access_table.at(data_block_address).access_stats[data_line_positon]          = true;
    access_table.at(data_block_address).temporal_access_stats[data_line_positon] = true;
#endif // STATISTICS_INFORMATION

#if (COLD_DATA_DETECTION_IN_GROUP == ENABLE)
    cold_data_detection_in_group(address);
#endif // COLD_DATA_DETECTION_IN_GROUP

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

    // Prepare a remapping request
    RemappingRequest remapping_request;

    // This data block is hot and belongs to slow memory
    if ((hotness_table.at(data_block_address) == true) && (tag != REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)))
    {
        // Calculate the free space in fast memory for this set
        int16_t free_space = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4);
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            // Traverse the placement entry to calculate the free space in fast memory.
            free_space -= placement_table.at(placement_table_index).granularity[i];
        }

        // Sanity check
        if (free_space < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None))
        {
            std::cout << __func__ << ": free_space calculation error." << std::endl;
            for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
            {
                printf("tag[%d]: %d, ", i, placement_table.at(placement_table_index).tag[i]);
                printf("start_address[%d]: %d, ", i, placement_table.at(placement_table_index).start_address[i]);
                printf("granularity[%d]: %d.\n", i, placement_table.at(placement_table_index).granularity[i]);
            }
            assert(false);
        }

        // Calculate the start address
        START_ADDRESS_WIDTH start_address = START_ADDRESS_WIDTH(StartAddress::Zero);
        for (START_ADDRESS_WIDTH i = START_ADDRESS_WIDTH(StartAddress::Zero); i < START_ADDRESS_WIDTH(StartAddress::Max); i++)
        {
            if (access_table.at(data_block_address).access[i] == true)
            {
                start_address = i;
                break;
            }
        }

        // Calculate the end address
        START_ADDRESS_WIDTH end_address = START_ADDRESS_WIDTH(StartAddress::Zero);
        for (START_ADDRESS_WIDTH i = START_ADDRESS_WIDTH(StartAddress::Max) - 1; i > START_ADDRESS_WIDTH(StartAddress::Zero); i--)
        {
            if (access_table.at(data_block_address).access[i] == true)
            {
                end_address = i;
                break;
            }
        }

        MIGRATION_GRANULARITY_WIDTH migration_granularity = calculate_migration_granularity(start_address, end_address);

        end_address                                       = adjust_migration_granularity(start_address, end_address, migration_granularity);

        // Check placement metadata to find if this data block is already in fast memory
        bool is_expanded                                  = false;
        bool is_limited                                   = false; // Existing groups could limit end_address and/or start_address
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            REMAPPING_LOCATION_WIDTH data_block_position     = 0;
            START_ADDRESS_WIDTH existing_group_start_address = START_ADDRESS_WIDTH(StartAddress::Zero);
            START_ADDRESS_WIDTH existing_group_end_address   = START_ADDRESS_WIDTH(StartAddress::Zero);

            // Traverse the placement entry to find its tag if it exists. (checking multiple groups with same tag is possible)
            if (placement_table.at(placement_table_index).tag[i] == tag)
            {
                // Record the information of this existing group
                data_block_position          = i;
                existing_group_start_address = placement_table.at(placement_table_index).start_address[data_block_position];
                existing_group_end_address   = placement_table.at(placement_table_index).start_address[data_block_position] + placement_table.at(placement_table_index).granularity[data_block_position] - 1;

                // Check the position of this existing group, note here the cursor won't be zero
                if ((placement_table.at(placement_table_index).cursor - 1) != data_block_position)
                {
#if (FLEXIBLE_DATA_PLACEMENT == ENABLE)
                    if ((start_address <= existing_group_end_address) && (existing_group_end_address < end_address))
                    {
                        // Case one: the front of new hot data is overlapped with this existing group
                        start_address = existing_group_end_address + 1;
                        if (is_limited)
                        {
                            end_address = round_down_migration_granularity(start_address, end_address, migration_granularity);
                        }
                        else
                        {
                            end_address = adjust_migration_granularity(start_address, end_address, migration_granularity);
                        }
                    }
                    else if ((start_address < existing_group_start_address) && (existing_group_end_address < end_address))
                    {
                        // Case two: new hot data contain the data of this existing group
                        is_limited  = true;
                        end_address = existing_group_start_address - 1; // throw the rear of the new hot data

                        end_address = round_down_migration_granularity(start_address, end_address, migration_granularity);
                    }
                    else if ((existing_group_start_address <= start_address) && (end_address <= existing_group_end_address))
                    {
                        // Case three: hit in fast memory
                        return true;
                    }
                    else if ((start_address < existing_group_start_address) && (existing_group_start_address <= end_address))
                    {
                        // Case four: the rear of new hot data is overlapped with this existing group
                        is_limited  = true;
                        end_address = existing_group_start_address - 1; // throw the rear of the new hot data

                        end_address = round_down_migration_granularity(start_address, end_address, migration_granularity);
                    }
                    else
                    {
                        // Case five: no data overlapping
                    }

                    // This existing group can not be expanded because no invalid groups behind it (i.e., no continuous free space).
                    output_statistics.unexpandable_since_no_invalid_group++;
#else
                    // This existing group can not be expanded because no invalid groups behind it (i.e., no continuous free space).
                    cold_data_eviction(address, queue_busy_degree);
                    output_statistics.unexpandable_since_no_invalid_group++;
                    return true;
#endif // FLEXIBLE_DATA_PLACEMENT
                }
                else
                {
                    // Right in the front of the position pointed by the cursor

                    if (start_address < existing_group_start_address)
                    {
#if (FLEXIBLE_DATA_PLACEMENT == ENABLE)
                        // Case one: this existing group can't be expanded
                        is_limited  = true;
                        end_address = existing_group_start_address - 1; // Throw the rear of the new hot data

                        end_address = round_down_migration_granularity(start_address, end_address, migration_granularity);
                        break;
#else
                        // This existing group can not be expanded because the new start_address is smaller than the existing group's start_address
                        output_statistics.unexpandable_since_start_address++;
                        return true;
#endif // FLEXIBLE_DATA_PLACEMENT
                    }
                    else
                    {
                        // Case two: this existing group can be expanded
                        // Synchronize their start addresses
                        start_address = existing_group_start_address;

                        if (existing_group_end_address < end_address)
                        {
                            // New hot data are needed to migrate
                            // Check whether this new migration_granularity is possible to track in the remapping table
                            migration_granularity = calculate_migration_granularity(start_address, end_address);

                            if (is_limited)
                            {
                                end_address = round_down_migration_granularity(start_address, end_address, migration_granularity);
                            }
                            else
                            {
                                end_address = adjust_migration_granularity(start_address, end_address, migration_granularity);
                            }

                            if (placement_table.at(placement_table_index).granularity[data_block_position] < migration_granularity)
                            {
                                MIGRATION_GRANULARITY_WIDTH remain_hot_data = migration_granularity - placement_table.at(placement_table_index).granularity[data_block_position];

                                // Chech whether there has enough free space
                                if (remain_hot_data <= free_space)
                                {
                                    // This existing group is needed to be expanded
                                    is_expanded           = true;
                                    migration_granularity = remain_hot_data;

                                    // This should be equal to placement_table.at(placement_table_index).start_address[data_block_position] + placement_table.at(placement_table_index).granularity[data_block_position]
                                    start_address         = (end_address + 1) - remain_hot_data;
                                    break;
                                }
                                else
                                {
#if (FLEXIBLE_GRANULARITY == ENABLE)
                                    if (free_space)
                                    {
                                        // Free_space is not empty
                                        // This existing group is needed to be expanded
                                        is_expanded           = true;
                                        migration_granularity = free_space;

                                        // This should be equal to placement_table.at(placement_table_index).start_address[data_block_position] + placement_table.at(placement_table_index).granularity[data_block_position]
                                        start_address         = (end_address + 1) - free_space;
                                        break;
                                    }
                                    else
                                    {
                                        // No enough free space for data migration. Data eviction is necessary.
                                        cold_data_eviction(address, queue_busy_degree);
                                        output_statistics.no_free_space_for_migration++;
                                        return true;
                                    }

#else
                                    // No enough free space for data migration. Data eviction is necessary.
                                    cold_data_eviction(address, queue_busy_degree);
                                    output_statistics.no_free_space_for_migration++;
                                    return true;
#endif // FLEXIBLE_GRANULARITY
                                }
                            }
                            else
                            {
                                // Not need to migrate hot data because no need to update the migration_granularity. (hit in fast/slow memory)
                                return true;
                            }
                        }
                        else
                        {
                            // Not need to migrate hot data because the data are already in fast memory. (hit in fast memory)
                            return true;
                        }
                    }
                }
            }
        }

        // Sanity check
        if (migration_granularity == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None))
        {
            std::cout << __func__ << ": migration granularity calculation error." << std::endl;
            assert(false);
        }

        if (is_expanded) // This data block can be expanded in fast memory
        {
            // No need to chech whether there has enough free space
            // No need to check the cursor
        }
        else
        {
            // This data block can not be expanded in fast memory (new to or part of it is in fast memory)
            // Check whether there has enough free space
            if (migration_granularity <= free_space)
            {
                // Check whether there have enough invalid groups in placement table entry for new migration
                if (placement_table.at(placement_table_index).cursor == NUMBER_OF_BLOCK)
                {
                    // No enough invalid groups for data migration. Data eviction is necessary.
                    cold_data_eviction(address, queue_busy_degree);
                    output_statistics.no_invalid_group_for_migration++;
                    return true;
                }
            }
            else
            {
#if (FLEXIBLE_GRANULARITY == ENABLE)
                if (free_space)
                {
                    // Free_space is not empty
                    // Check whether there have enough invalid groups in placement table entry for new migration
                    if (placement_table.at(placement_table_index).cursor == NUMBER_OF_BLOCK)
                    {
                        // No enough invalid groups for data migration. Data eviction is necessary.
                        cold_data_eviction(address, queue_busy_degree);
                        output_statistics.no_invalid_group_for_migration++;
                        return true;
                    }
                    migration_granularity = free_space;
                }
                else
                {
                    // No enough free space for data migration. Data eviction is necessary.
                    cold_data_eviction(address, queue_busy_degree);
                    output_statistics.no_free_space_for_migration++;
                    return true;
                }
#else
                // No enough free space for data migration. Data eviction is necessary.
                cold_data_eviction(address, queue_busy_degree);
                output_statistics.no_free_space_for_migration++;
                return true;
#endif // FLEXIBLE_GRANULARITY
            }
        }

        // This should be RemappingLocation::Zero.
        REMAPPING_LOCATION_WIDTH fm_location    = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero);

        // Follow rule 2 (data blocks belonging to NM are recovered to the original locations)
        START_ADDRESS_WIDTH start_address_in_fm = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4) - free_space;

        remapping_request.address_in_fm         = champsim::replace_bits(base_remapping_address + (start_address_in_fm << DATA_LINE_OFFSET_BITS), uint64_t(fm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);
        remapping_request.address_in_sm         = champsim::replace_bits(base_remapping_address + (start_address << DATA_LINE_OFFSET_BITS), uint64_t(tag) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);

        // Indicate where the data come from for address_in_fm and address_in_sm. (What block the data belong to)
        remapping_request.fm_location           = fm_location; // This should be RemappingLocation::Zero.
        remapping_request.sm_location           = tag;         // This shouldn't be RemappingLocation::Zero.

        remapping_request.size                  = migration_granularity;
    }
    else if ((hotness_table.at(data_block_address) == false) && (tag != REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)))
    {
        // This data block is cold and belongs to slow memory

        // Check placement metadata to find if the data of this data block is already in fast memory
        bool is_hit = false; // Hit in fast memory
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            REMAPPING_LOCATION_WIDTH data_block_position     = 0;
            START_ADDRESS_WIDTH existing_group_start_address = START_ADDRESS_WIDTH(StartAddress::Zero);
            START_ADDRESS_WIDTH existing_group_end_address   = START_ADDRESS_WIDTH(StartAddress::Zero);

            // Traverse the placement entry to find its tag if it exists. (checking multiple groups with same tag is possible)
            if (placement_table.at(placement_table_index).tag[i] == tag)
            {
                // Record the information of this existing group
                data_block_position          = i;
                existing_group_start_address = placement_table.at(placement_table_index).start_address[data_block_position];
                existing_group_end_address   = placement_table.at(placement_table_index).start_address[data_block_position] + placement_table.at(placement_table_index).granularity[data_block_position] - 1;

                if ((existing_group_start_address <= data_line_positon) && (data_line_positon <= existing_group_end_address))
                {
                    // Hit in fast memory
                    is_hit = true;
                    break;
                }
            }
        }

        if (is_hit) // The data of this data block is already in fast memory
        {
            // Hit in fast memory
            /* No code here */
        }
        else
        {
            // Hit in slow memory
            cold_data_eviction(address, queue_busy_degree);
        }
        return true;
    }
    else if (tag == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)) // This data block belongs to fast memory.
    {
        // Calculate the start address
        START_ADDRESS_WIDTH start_address              = data_line_positon;

        // Check whether this cache line is already in fast memory.
        bool in_fm                                     = true;
        REMAPPING_LOCATION_WIDTH occupied_group_number = 0;
        MIGRATION_GRANULARITY_WIDTH used_space         = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            used_space += placement_table.at(placement_table_index).granularity[i];
            START_ADDRESS_WIDTH accumulated_group_end_address = used_space - 1;

            if (start_address <= accumulated_group_end_address)
            {
                if (placement_table.at(placement_table_index).tag[i] == tag)
                {
                    // This cache line is already in fast memory
                    in_fm = true;
                    break;
                }
                else
                {
                    // This cache line is in slow memory
                    in_fm                 = false;
                    occupied_group_number = i;
                    used_space -= placement_table.at(placement_table_index).granularity[occupied_group_number];
                    break;
                }
            }
        }

        if (in_fm)
        {
            // No need for migration
            return true;
        }
        else
        {
            REMAPPING_LOCATION_WIDTH sm_location    = placement_table.at(placement_table_index).tag[occupied_group_number];

            // Follow rule 2 (data blocks belonging to NM are recovered to the original locations)
            START_ADDRESS_WIDTH start_address_in_fm = used_space;
            start_address                           = placement_table.at(placement_table_index).start_address[occupied_group_number];

            remapping_request.address_in_fm         = champsim::replace_bits(base_remapping_address + (start_address_in_fm << DATA_LINE_OFFSET_BITS), uint64_t(tag) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);
            remapping_request.address_in_sm         = champsim::replace_bits(base_remapping_address + (start_address << DATA_LINE_OFFSET_BITS), uint64_t(sm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);

            // Indicate where the data come from for address_in_fm and address_in_sm. (What block the data belong to)
            remapping_request.fm_location           = sm_location; // This shouldn't be RemappingLocation::Zero.
            remapping_request.sm_location           = tag;         // This should be RemappingLocation::Zero.

            remapping_request.size                  = placement_table.at(placement_table_index).granularity[occupied_group_number];
        }
    }
    else
    {
        return true;
    }

    if (queue_busy_degree <= QUEUE_BUSY_DEGREE_THRESHOLD)
    {
        enqueue_remapping_request(remapping_request);
    }

    return true;
};

void OS_TRANSPARENT_MANAGEMENT::physical_to_hardware_address(request_type& packet)
{
    uint64_t data_block_address           = packet.address >> DATA_MANAGEMENT_OFFSET_BITS;
    uint64_t placement_table_index        = data_block_address % fast_memory_capacity_at_data_block_granularity;
    uint64_t base_remapping_address       = placement_table_index << DATA_MANAGEMENT_OFFSET_BITS;
    REMAPPING_LOCATION_WIDTH tag          = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity);
    uint64_t first_address                = data_block_address << DATA_MANAGEMENT_OFFSET_BITS;
    START_ADDRESS_WIDTH data_line_positon = START_ADDRESS_WIDTH((packet.address >> DATA_LINE_OFFSET_BITS) - (first_address >> DATA_LINE_OFFSET_BITS));

    if (tag != REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero))
    {
        // This data block belongs to slow memory
        // Check placement metadata to find if the data of this data block is already in fast memory
        bool is_hit                                  = false; // Hit in fast memory
        REMAPPING_LOCATION_WIDTH data_block_position = 0;
        MIGRATION_GRANULARITY_WIDTH used_space       = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            START_ADDRESS_WIDTH existing_group_start_address = START_ADDRESS_WIDTH(StartAddress::Zero);
            START_ADDRESS_WIDTH existing_group_end_address   = START_ADDRESS_WIDTH(StartAddress::Zero);

            // Traverse the placement entry to find its tag if it exists. (checking multiple groups with same tag is possible)
            if (placement_table.at(placement_table_index).tag[i] == tag)
            {
                // Record the information of this existing group
                data_block_position          = i;
                existing_group_start_address = placement_table.at(placement_table_index).start_address[data_block_position];
                existing_group_end_address   = placement_table.at(placement_table_index).start_address[data_block_position] + placement_table.at(placement_table_index).granularity[data_block_position] - 1;

                if ((existing_group_start_address <= data_line_positon) && (data_line_positon <= existing_group_end_address))
                {
                    // Hit in fast memory
                    is_hit = true;
                    break;
                }
            }

            used_space += placement_table.at(placement_table_index).granularity[i];
        }

        if (is_hit)
        {
            // The data of this physical address is in fast memory
            // Calculate the start address
            START_ADDRESS_WIDTH start_address    = used_space + data_line_positon - placement_table.at(placement_table_index).start_address[data_block_position];
            REMAPPING_LOCATION_WIDTH fm_location = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero);
            packet.h_address                     = champsim::replace_bits(champsim::replace_bits(base_remapping_address + (start_address << DATA_LINE_OFFSET_BITS), uint64_t(fm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit), packet.address, DATA_LINE_OFFSET_BITS - 1);
        }
        else
        {
            // The data of this physical address is in slow memory, no need for translation
            packet.h_address = packet.address;
        }
    }
    else
    {
        // This data block belongs to fast memory
        // Calculate the start address
        START_ADDRESS_WIDTH start_address              = data_line_positon;

        // Check whether this cache line is already in fast memory.
        bool in_fm                                     = true;
        REMAPPING_LOCATION_WIDTH occupied_group_number = 0;
        MIGRATION_GRANULARITY_WIDTH used_space         = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            used_space += placement_table.at(placement_table_index).granularity[i];
            START_ADDRESS_WIDTH accumulated_group_end_address = used_space - 1;

            if (start_address <= accumulated_group_end_address)
            {
                if (placement_table.at(placement_table_index).tag[i] == tag)
                {
                    // This cache line is already in fast memory
                    in_fm = true;
                    break;
                }
                else
                {
                    // This cache line is in slow memory
                    in_fm                 = false;
                    occupied_group_number = i;
                    used_space -= placement_table.at(placement_table_index).granularity[occupied_group_number];
                    break;
                }
            }
        }

        if (in_fm)
        {
            // The data of this physical address is in fast memory, no need for translaton
            packet.h_address = packet.address;
        }
        else
        {
            // The data of this physical address is in slow memory
            REMAPPING_LOCATION_WIDTH sm_location = placement_table.at(placement_table_index).tag[occupied_group_number];
            start_address                        = placement_table.at(placement_table_index).start_address[occupied_group_number] + start_address - used_space;

            packet.h_address                     = champsim::replace_bits(champsim::replace_bits(base_remapping_address + (start_address << DATA_LINE_OFFSET_BITS), uint64_t(sm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit), packet.address, DATA_LINE_OFFSET_BITS - 1);
        }
    }
};

void OS_TRANSPARENT_MANAGEMENT::physical_to_hardware_address(uint64_t& address)
{
    uint64_t data_block_address           = address >> DATA_MANAGEMENT_OFFSET_BITS;
    uint64_t placement_table_index        = data_block_address % fast_memory_capacity_at_data_block_granularity;
    uint64_t base_remapping_address       = placement_table_index << DATA_MANAGEMENT_OFFSET_BITS;
    REMAPPING_LOCATION_WIDTH tag          = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity);
    uint64_t first_address                = data_block_address << DATA_MANAGEMENT_OFFSET_BITS;
    START_ADDRESS_WIDTH data_line_positon = START_ADDRESS_WIDTH((address >> DATA_LINE_OFFSET_BITS) - (first_address >> DATA_LINE_OFFSET_BITS));

    if (tag != REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero))
    {
        // This data block belongs to slow memory
        // Check placement metadata to find if the data of this data block is already in fast memory
        bool is_hit                                  = false; // Hit in fast memory
        REMAPPING_LOCATION_WIDTH data_block_position = 0;
        MIGRATION_GRANULARITY_WIDTH used_space       = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            START_ADDRESS_WIDTH existing_group_start_address = START_ADDRESS_WIDTH(StartAddress::Zero);
            START_ADDRESS_WIDTH existing_group_end_address   = START_ADDRESS_WIDTH(StartAddress::Zero);

            // Traverse the placement entry to find its tag if it exists. (checking multiple groups with same tag is possible)
            if (placement_table.at(placement_table_index).tag[i] == tag)
            {
                // Record the information of this existing group
                data_block_position          = i;
                existing_group_start_address = placement_table.at(placement_table_index).start_address[data_block_position];
                existing_group_end_address   = placement_table.at(placement_table_index).start_address[data_block_position] + placement_table.at(placement_table_index).granularity[data_block_position] - 1;

                if ((existing_group_start_address <= data_line_positon) && (data_line_positon <= existing_group_end_address))
                {
                    // Hit in fast memory
                    is_hit = true;
                    break;
                }
            }

            used_space += placement_table.at(placement_table_index).granularity[i];
        }

        if (is_hit)
        {
            // The data of this physical address is in fast memory
            // Calculate the start address
            START_ADDRESS_WIDTH start_address    = used_space + data_line_positon - placement_table.at(placement_table_index).start_address[data_block_position];
            REMAPPING_LOCATION_WIDTH fm_location = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero);
            address                              = champsim::replace_bits(champsim::replace_bits(base_remapping_address + (start_address << DATA_LINE_OFFSET_BITS), uint64_t(fm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit), address, DATA_LINE_OFFSET_BITS - 1);
        }
        else
        {
            // The data of this physical address is in slow memory, no need for translation
            address = address;
        }
    }
    else
    {
        // This data block belongs to fast memory
        // Calculate the start address
        START_ADDRESS_WIDTH start_address              = data_line_positon;

        // Check whether this cache line is already in fast memory.
        bool in_fm                                     = true;
        REMAPPING_LOCATION_WIDTH occupied_group_number = 0;
        MIGRATION_GRANULARITY_WIDTH used_space         = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
        for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
        {
            used_space += placement_table.at(placement_table_index).granularity[i];
            START_ADDRESS_WIDTH accumulated_group_end_address = used_space - 1;

            if (start_address <= accumulated_group_end_address)
            {
                if (placement_table.at(placement_table_index).tag[i] == tag)
                {
                    // This cache line is already in fast memory
                    in_fm = true;
                    break;
                }
                else
                {
                    // This cache line is in slow memory
                    in_fm                 = false;
                    occupied_group_number = i;
                    used_space -= placement_table.at(placement_table_index).granularity[occupied_group_number];
                    break;
                }
            }
        }

        if (in_fm)
        {
            // The data of this physical address is in fast memory, no need for translaton
            address = address;
        }
        else
        {
            // The data of this physical address is in slow memory
            REMAPPING_LOCATION_WIDTH sm_location = placement_table.at(placement_table_index).tag[occupied_group_number];
            start_address                        = placement_table.at(placement_table_index).start_address[occupied_group_number] + start_address - used_space;

            address                              = champsim::replace_bits(champsim::replace_bits(base_remapping_address + (start_address << DATA_LINE_OFFSET_BITS), uint64_t(sm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit), address, DATA_LINE_OFFSET_BITS - 1);
        }
    }
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

        uint64_t data_block_address    = remapping_request.address_in_fm >> DATA_MANAGEMENT_OFFSET_BITS;
        //data_block_address = remapping_request.address_in_sm >> DATA_MANAGEMENT_OFFSET_BITS;
        uint64_t placement_table_index = data_block_address % fast_memory_capacity_at_data_block_granularity;

        // Check whether the remapping_request moves block 0's data into fast memory
        if (remapping_request.fm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero))
        {
            // This remapping_request moves block 0's data into slow memory

            data_block_address                           = remapping_request.address_in_sm >> DATA_MANAGEMENT_OFFSET_BITS;
            REMAPPING_LOCATION_WIDTH tag                 = remapping_request.sm_location;
            START_ADDRESS_WIDTH start_address            = (remapping_request.address_in_sm >> DATA_LINE_OFFSET_BITS) % (START_ADDRESS_WIDTH(StartAddress::Max));

            // Check whether part of this data block can be expanded in fast memory
            REMAPPING_LOCATION_WIDTH data_block_position = 0;
            bool is_expanded                             = false;
            if (placement_table.at(placement_table_index).cursor > 0)
            {
                if (placement_table.at(placement_table_index).tag[placement_table.at(placement_table_index).cursor - 1] == tag)
                {
                    // Record the information of this existing group
                    data_block_position                              = placement_table.at(placement_table_index).cursor - 1;
                    START_ADDRESS_WIDTH existing_group_start_address = placement_table.at(placement_table_index).start_address[data_block_position];

                    if (existing_group_start_address <= start_address)
                    {
                        is_expanded = true;
                    }
                    else
                    {
#if (FLEXIBLE_DATA_PLACEMENT == ENABLE)
#else
                        std::cout << __func__ << ": start address calculation error." << std::endl;
                        printf("existing_group_start_address: %d, ", existing_group_start_address);
                        printf("start_address: %d.\n", start_address);
                        assert(false);
#endif // FLEXIBLE_DATA_PLACEMENT
                    }
                }
            }

            if (is_expanded)
            {
                // Expand this existing group

#if (STATISTICS_INFORMATION == ENABLE)
                if (placement_table.at(placement_table_index).granularity[data_block_position] == access_table.at(data_block_address).granularity_predict_stats)
                {
                    access_table.at(data_block_address).granularity_predict_stats += remapping_request.size;
                }
#endif // STATISTICS_INFORMATION

                // Fill granularity in placement table entry
                placement_table.at(placement_table_index).granularity[data_block_position] += remapping_request.size;

#if (FLEXIBLE_GRANULARITY == ENABLE)
#else
                // Sanity check
                if (placement_table.at(placement_table_index).granularity[data_block_position] == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128))
                {
                    /* Code */
                }
                else if (placement_table.at(placement_table_index).granularity[data_block_position] == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256))
                {
                    /* Code */
                }
                else if (placement_table.at(placement_table_index).granularity[data_block_position] == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512))
                {
                    /* Code */
                }
                else if (placement_table.at(placement_table_index).granularity[data_block_position] == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1))
                {
                    /* Code */
                }
                else if (placement_table.at(placement_table_index).granularity[data_block_position] == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2))
                {
                    /* Code */
                }
                else if (placement_table.at(placement_table_index).granularity[data_block_position] == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4))
                {
                    /* Code */
                }
                else
                {
                    printf("placement_table.at(%ld).granularity[%d] is %d.\n", placement_table_index, data_block_position, placement_table.at(placement_table_index).granularity[data_block_position]);
                    printf("remapping_request.size is %d.\n", remapping_request.size);
                    printf("tag is %d.\n", tag);
                    assert(false);
                }
#endif // FLEXIBLE_GRANULARITY
            }
            else
            {
#if (STATISTICS_INFORMATION == ENABLE)
                if (remapping_request.size > access_table.at(data_block_address).granularity_predict_stats)
                {
                    // Record the biggest predicted granularity
                    access_table.at(data_block_address).granularity_predict_stats = remapping_request.size;
                }
#endif // STATISTICS_INFORMATION

                data_block_position                                                          = placement_table.at(placement_table_index).cursor;

                // Fill tag in placement table entry
                placement_table.at(placement_table_index).tag[data_block_position]           = remapping_request.sm_location;

                // Fill start_address in placement table entry
                placement_table.at(placement_table_index).start_address[data_block_position] = start_address;

                // Fill granularity in placement table entry
                placement_table.at(placement_table_index).granularity[data_block_position]   = remapping_request.size;

                // Remember to update the cursor
                if (placement_table.at(placement_table_index).cursor < NUMBER_OF_BLOCK)
                {
                    placement_table.at(placement_table_index).cursor++;
                }
                else
                {
                    std::cout << __func__ << ": cursor calculation error." << std::endl;
                    assert(false);
                }

                // Calculate the free space in fast memory for this set
                int16_t free_space = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4);
                for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
                {
                    // Traverse the placement entry to calculate the free space in fast memory.
                    free_space -= placement_table.at(placement_table_index).granularity[i];
                }

                // Sanity check
                if (free_space < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None))
                {
                    std::cout << __func__ << ": free_space calculation error." << std::endl;
                    for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
                    {
                        std::printf("tag[%d]: %d, ", i, placement_table.at(placement_table_index).tag[i]);
                        std::printf("start_address[%d]: %d, ", i, placement_table.at(placement_table_index).start_address[i]);
                        std::printf("granularity[%d]: %d.\n", i, placement_table.at(placement_table_index).granularity[i]);
                    }
                    assert(false);
                }
            }
        }
        else if (remapping_request.sm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero))
        {
            // This remapping_request moves block 0's data into fast memory
            START_ADDRESS_WIDTH start_address_in_fm        = (remapping_request.address_in_fm >> DATA_LINE_OFFSET_BITS) % (START_ADDRESS_WIDTH(StartAddress::Max));
            START_ADDRESS_WIDTH start_address              = (remapping_request.address_in_sm >> DATA_LINE_OFFSET_BITS) % (START_ADDRESS_WIDTH(StartAddress::Max));

            bool find_occupied_group                       = false;
            REMAPPING_LOCATION_WIDTH occupied_group_number = 0;
            MIGRATION_GRANULARITY_WIDTH used_space         = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
            for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
            {
                used_space += placement_table.at(placement_table_index).granularity[i];
                START_ADDRESS_WIDTH accumulated_group_end_address = used_space - 1;

                // Find the group in correct position
                if (start_address_in_fm <= accumulated_group_end_address)
                {
                    if (placement_table.at(placement_table_index).tag[i] == remapping_request.fm_location)
                    {
                        // Sanity check
                        if (placement_table.at(placement_table_index).granularity[i] != remapping_request.size)
                        {
                            std::cout << __func__ << ": migration granularity calculation error." << std::endl;
                            assert(false);
                        }

                        find_occupied_group   = true;
                        occupied_group_number = i;
                        break;
                    }
                }
            }

            if (find_occupied_group == false)
            {
                std::cout << __func__ << ": occupied_group_number calculation error." << std::endl;
                for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
                {
                    std::printf("tag[%d]: %d, ", i, placement_table.at(placement_table_index).tag[i]);
                    std::printf("start_address[%d]: %d, ", i, placement_table.at(placement_table_index).start_address[i]);
                    std::printf("granularity[%d]: %d.\n", i, placement_table.at(placement_table_index).granularity[i]);
                }
                std::printf("\nfm_location: %d, ", remapping_request.fm_location);
                std::printf("sm_location: %d, ", remapping_request.sm_location);
                std::printf("size: %d.\n", remapping_request.size);
                std::printf("start_address_in_fm: %d, ", start_address_in_fm);
                std::printf("start_address: %d.\n", start_address);
                assert(false);
            }

            // Fill tag in placement table entry
            placement_table.at(placement_table_index).tag[occupied_group_number]           = remapping_request.sm_location;

            // Fill start_address in placement table entry
            placement_table.at(placement_table_index).start_address[occupied_group_number] = start_address_in_fm;

            // Fill granularity in placement table entry
            placement_table.at(placement_table_index).granularity[occupied_group_number]   = remapping_request.size;

            // Check and mark invalid groups
            bool is_invalid                                                                = false;
            if ((occupied_group_number + 1) == placement_table.at(placement_table_index).cursor)
            {
                placement_table.at(placement_table_index).granularity[occupied_group_number] = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
                placement_table.at(placement_table_index).cursor                             = occupied_group_number;
                is_invalid                                                                   = true;
            }

            if (is_invalid)
            {
                for (REMAPPING_LOCATION_WIDTH_SIGN i = occupied_group_number - 1; i >= 0; i--) // Go forward to check and mark invalid groups
                {
                    if (placement_table.at(placement_table_index).tag[i] == remapping_request.sm_location)
                    {
                        placement_table.at(placement_table_index).granularity[i] = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
                        placement_table.at(placement_table_index).cursor         = i;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            // Calculate the free space in fast memory for this set
            int16_t free_space = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4);
            for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
            {
                // Traverse the placement entry to calculate the free space in fast memory.
                free_space -= placement_table.at(placement_table_index).granularity[i];
            }

            // Sanity check
            if (free_space < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None))
            {
                std::cout << __func__ << ": free_space calculation error 2." << std::endl;
                for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
                {
                    std::printf("tag[%d]: %d, ", i, placement_table.at(placement_table_index).tag[i]);
                    std::printf("start_address[%d]: %d, ", i, placement_table.at(placement_table_index).start_address[i]);
                    std::printf("granularity[%d]: %d.\n", i, placement_table.at(placement_table_index).granularity[i]);
                }
                assert(false);
            }
        }
        else
        {
            std::cout << __func__ << ": fm_location calculation error." << std::endl;
            assert(false);
        }
    }
    else
    {
        std::cout << __func__ << ": remapping queue empty error." << std::endl;
        assert(false);
        return false; // Error
    }

    return true;
};

void OS_TRANSPARENT_MANAGEMENT::cold_data_detection()
{
    if ((cycle % INTERVAL_FOR_DECREMENT) == 0)
    {
#if (IMMEDIATE_EVICTION == ENABLE)
#else
        // Overhead here is heavy. OpenMP is necessary.
#if (USE_OPENMP == ENABLE)
#pragma omp parallel
        {
#pragma omp for
#endif // USE_OPENMP
            for (uint64_t i = 0; i < total_capacity_at_data_block_granularity; i++)
            {
                counter_table[i] >>= 1; // Halve the counter value
                if (counter_table[i] == 0)
                {
                    hotness_table.at(i) = false; // Mark cold data block
                    for (START_ADDRESS_WIDTH j = START_ADDRESS_WIDTH(StartAddress::Zero); j < START_ADDRESS_WIDTH(StartAddress::Max); j++)
                    {
                        // Clear accessed data line
                        access_table.at(i).access[j] = false;
                    }
                }

#if (STATISTICS_INFORMATION == ENABLE)
                // Count the estimated spatial locality
                START_ADDRESS_WIDTH count_access = START_ADDRESS_WIDTH(StartAddress::Zero);
                for (START_ADDRESS_WIDTH j = START_ADDRESS_WIDTH(StartAddress::Zero); j < START_ADDRESS_WIDTH(StartAddress::Max); j++)
                {
                    if (access_table.at(i).temporal_access_stats[j] == true)
                    {
                        count_access++;
                        access_table.at(i).temporal_access_stats[j] = false; // Clear this bit
                    }
                }

                if (access_table.at(i).estimated_spatial_locality_stats < count_access)
                {
                    // Store the biggest estimated spatial locality
                    access_table.at(i).estimated_spatial_locality_stats = count_access;
                }
#endif // STATISTICS_INFORMATION
            }
#if (USE_OPENMP == ENABLE)
        }
#endif // USE_OPENMP
#endif // IMMEDIATE_EVICTION
    }

    cycle++;
}

#if (COLD_DATA_DETECTION_IN_GROUP == ENABLE)
void OS_TRANSPARENT_MANAGEMENT::cold_data_detection_in_group(uint64_t source_address)
{
    uint64_t data_block_address     = source_address >> DATA_MANAGEMENT_OFFSET_BITS;                       // Calculate the data block address
    uint64_t placement_table_index  = data_block_address % fast_memory_capacity_at_data_block_granularity; // Calculate the index in placement table
    uint64_t base_remapping_address = placement_table_index << DATA_MANAGEMENT_OFFSET_BITS;
    REMAPPING_LOCATION_WIDTH tag    = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity); // Calculate the tag of the data block address

    for (REMAPPING_LOCATION_WIDTH i = 0; i < expected_number_in_congruence_group; i++)
    {
        if (i != tag)
        {
            REMAPPING_LOCATION_WIDTH location    = i;
            uint64_t data_base_address_to_evict  = champsim::replace_bits(base_remapping_address, uint64_t(location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);
            uint64_t data_block_address_to_evict = data_base_address_to_evict >> DATA_MANAGEMENT_OFFSET_BITS;

            counter_table[data_block_address_to_evict] >>= 1; // Halve the counter value
            if (counter_table[data_block_address_to_evict] == 0)
            {
                hotness_table.at(data_block_address_to_evict) = false; // Mark cold data block
                for (START_ADDRESS_WIDTH j = 0; j < START_ADDRESS_WIDTH(StartAddress::Max); j++)
                {
                    // Clear accessed data line
                    access_table.at(data_block_address_to_evict).access[j] = false;
                }
            }
        }
    }
}
#endif // COLD_DATA_DETECTION_IN_GROUP

bool OS_TRANSPARENT_MANAGEMENT::cold_data_eviction(uint64_t source_address, float queue_busy_degree)
{
#if (DATA_EVICTION == ENABLE)
    uint64_t data_block_address                    = source_address >> DATA_MANAGEMENT_OFFSET_BITS;                       // Calculate the data block address
    uint64_t placement_table_index                 = data_block_address % fast_memory_capacity_at_data_block_granularity; // Calculate the index in placement table
    uint64_t base_remapping_address                = placement_table_index << DATA_MANAGEMENT_OFFSET_BITS;
    REMAPPING_LOCATION_WIDTH tag                   = static_cast<REMAPPING_LOCATION_WIDTH>(data_block_address / fast_memory_capacity_at_data_block_granularity); // Calculate the tag of the data block address
    // uint64_t first_address = data_block_address << DATA_MANAGEMENT_OFFSET_BITS;
    // START_ADDRESS_WIDTH data_line_positon = START_ADDRESS_WIDTH((source_address >> DATA_LINE_OFFSET_BITS) - (first_address >> DATA_LINE_OFFSET_BITS));

    // Find cold data block and it belongs to slow memory in placement table entry
    bool is_cold                                   = false;
    REMAPPING_LOCATION_WIDTH occupied_group_number = 0;
    MIGRATION_GRANULARITY_WIDTH used_space         = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None);
    for (REMAPPING_LOCATION_WIDTH i = 0; i < placement_table.at(placement_table_index).cursor; i++)
    {
        if (placement_table.at(placement_table_index).granularity[i] == MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None))
        {
            std::cout << __func__ << ": used space calculation error." << std::endl;
            assert(false);
        }

        used_space += placement_table.at(placement_table_index).granularity[i];

        if (placement_table.at(placement_table_index).tag[i] == tag)
        {
            // Don't evict the data block which has same tag.
            continue;
        }
        else if (placement_table.at(placement_table_index).tag[i] != REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero))
        {
#if (IMMEDIATE_EVICTION == ENABLE)
            REMAPPING_LOCATION_WIDTH sm_location = placement_table.at(placement_table_index).tag[i];
            uint64_t data_base_address_to_evict  = champsim::replace_bits(base_remapping_address, uint64_t(sm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);
            uint64_t data_block_address_to_evict = data_base_address_to_evict >> DATA_MANAGEMENT_OFFSET_BITS;
            for (START_ADDRESS_WIDTH j = 0; j < START_ADDRESS_WIDTH(StartAddress::Max); j++)
            {
                // Clear accessed data line
                access_table.at(data_block_address_to_evict).access[j] = false;
            }

            is_cold               = true;
            occupied_group_number = i;
            used_space -= placement_table.at(placement_table_index).granularity[occupied_group_number];
            break;
#else
            // Check whether this data block is cold
            REMAPPING_LOCATION_WIDTH sm_location = placement_table.at(placement_table_index).tag[i];
            uint64_t data_base_address_to_evict  = champsim::replace_bits(base_remapping_address, uint64_t(sm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);
            uint64_t data_block_address_to_evict = data_base_address_to_evict >> DATA_MANAGEMENT_OFFSET_BITS;

            if (hotness_table.at(data_block_address_to_evict) == false) // This data block is cold
            {
                is_cold               = true;
                occupied_group_number = i;
                used_space -= placement_table.at(placement_table_index).granularity[occupied_group_number];
                break;
            }
#endif // IMMEDIATE_EVICTION
        }
    }

    if (is_cold)
    {
        for (REMAPPING_LOCATION_WIDTH i = occupied_group_number; i < placement_table.at(placement_table_index).cursor; i++)
        {
            // Check whether this tag is the tag of cold data block
            if (placement_table.at(placement_table_index).tag[i] == placement_table.at(placement_table_index).tag[occupied_group_number])
            {
                REMAPPING_LOCATION_WIDTH sm_location    = placement_table.at(placement_table_index).tag[i];

                // Follow rule 2 (data blocks belonging to NM are recovered to the original locations)
                START_ADDRESS_WIDTH start_address_in_fm = used_space;
                START_ADDRESS_WIDTH start_address       = placement_table.at(placement_table_index).start_address[i];

                tag                                     = REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero);

                // Prepare a remapping request
                RemappingRequest remapping_request;
                remapping_request.address_in_fm = champsim::replace_bits(base_remapping_address + (start_address_in_fm << DATA_LINE_OFFSET_BITS), uint64_t(tag) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);
                remapping_request.address_in_sm = champsim::replace_bits(base_remapping_address + (start_address << DATA_LINE_OFFSET_BITS), uint64_t(sm_location) << fast_memory_offset_bit, set_msb, fast_memory_offset_bit);

                // Indicate where the data come from for address_in_fm and address_in_sm.
                remapping_request.fm_location   = sm_location; // This shouldn't be RemappingLocation::Zero.
                remapping_request.sm_location   = tag;         // This should be RemappingLocation::Zero.

                remapping_request.size          = placement_table.at(placement_table_index).granularity[i];

                bool enqueue                    = false;
                if (queue_busy_degree <= QUEUE_BUSY_DEGREE_THRESHOLD)
                {
                    enqueue = enqueue_remapping_request(remapping_request);
                }

                if (enqueue)
                {
                    // New eviction request is issued.
                    output_statistics.data_eviction_success++;
                }
                else
                {
                    // No eviction request is issued.
                    output_statistics.data_eviction_failure++;
                }
            }

            // Prepare address calculation for next checking
            used_space += placement_table.at(placement_table_index).granularity[i];
        }
    }

#endif // DATA_EVICTION
    return true;
}

bool OS_TRANSPARENT_MANAGEMENT::enqueue_remapping_request(RemappingRequest& remapping_request)
{
    uint64_t data_block_address       = remapping_request.address_in_fm >> DATA_MANAGEMENT_OFFSET_BITS;
    uint64_t placement_table_index    = data_block_address % fast_memory_capacity_at_data_block_granularity;

    // Check duplicated remapping request in remapping_request_queue
    // If duplicated remapping requests exist, we won't add this new remapping request into the remapping_request_queue.
    bool duplicated_remapping_request = false;
    for (uint64_t i = 0; i < remapping_request_queue.size(); i++)
    {
        uint64_t data_block_address_to_check    = remapping_request_queue[i].address_in_fm >> DATA_MANAGEMENT_OFFSET_BITS;
        uint64_t placement_table_index_to_check = data_block_address_to_check % fast_memory_capacity_at_data_block_granularity;

        if (placement_table_index_to_check == placement_table_index)
        {
            duplicated_remapping_request = true; // Find a duplicated remapping request

            // Check whether the remapping_request moves block 0's data into fast memory
            if ((remapping_request.fm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)) && (remapping_request_queue[i].fm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)))
            {
                // For the request that moves block 0's data into slow memory, only one remapping request for the same set can exist in remapping_request_queue to maintain data consistency
                if ((remapping_request_queue[i].address_in_fm == remapping_request.address_in_fm) && ((remapping_request_queue[i].address_in_sm == remapping_request.address_in_sm)))
                {
                    if (remapping_request.size > remapping_request_queue[i].size)
                    {
                        remapping_request_queue[i].size = remapping_request.size; // Update size for data swapping
                    }

                    // New remapping request won't be issued, but the duplicated one is updated.
                    return true;
                }
            }
            else if ((remapping_request.sm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)) && (remapping_request_queue[i].sm_location == REMAPPING_LOCATION_WIDTH(RemappingLocation::Zero)))
            {
                // For the request that moves block 0's data into fast memory, multiple remapping requests for the same set can exist as long as the data movement in fast memory are different
                if ((remapping_request_queue[i].address_in_fm == remapping_request.address_in_fm) && (remapping_request_queue[i].address_in_sm == remapping_request.address_in_sm))
                {
                    if (remapping_request.size > remapping_request_queue[i].size)
                    {
                        remapping_request_queue[i].size = remapping_request.size; // Update size for data swapping
                    }

                    // New remapping request won't be issued, but the duplicated one is updated.
                    return true;
                }
                else if (remapping_request_queue[i].address_in_fm != remapping_request.address_in_fm)
                {
                    duplicated_remapping_request = false;
                    continue;
                }
            }

            break;
        }
    }

    // Add new remapping request to queue
    if (duplicated_remapping_request == false)
    {
        if (remapping_request_queue.size() < REMAPPING_REQUEST_QUEUE_LENGTH)
        {
            if (remapping_request.address_in_fm == remapping_request.address_in_sm) // Check
            {
                std::cout << __func__ << ": add new remapping request error." << std::endl;
                abort();
            }

            // Enqueue a remapping request
            remapping_request_queue.push_back(remapping_request);
        }
        else
        {
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

MIGRATION_GRANULARITY_WIDTH OS_TRANSPARENT_MANAGEMENT::calculate_migration_granularity(const START_ADDRESS_WIDTH start_address, const START_ADDRESS_WIDTH end_address)
{
    // Check
    if (start_address > end_address)
    {
        std::cout << __func__ << ": migration granularity calculation error." << std::endl;
        assert(false);
    }

    // Calculate the migration granularity
    MIGRATION_GRANULARITY_WIDTH migration_granularity = end_address - start_address;

    if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::None) <= migration_granularity) && (migration_granularity < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_64)))
    {
        migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_64);
    }
    else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_64) <= migration_granularity) && (migration_granularity < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128)))
    {
        migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128);
    }
    else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128) <= migration_granularity) && (migration_granularity < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256)))
    {
        migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256);
    }
    else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256) <= migration_granularity) && (migration_granularity < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512)))
    {
        migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512);
    }
    else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512) <= migration_granularity) && (migration_granularity < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1)))
    {
        migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1);
    }
    else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1) <= migration_granularity) && (migration_granularity < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2)))
    {
        migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2);
    }
    else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2) <= migration_granularity) && (migration_granularity < MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4)))
    {
        migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4);
    }
    else
    {
        std::cout << __func__ << ": migration granularity calculation error 2." << std::endl;
        assert(false);
    }

    return migration_granularity;
}

START_ADDRESS_WIDTH OS_TRANSPARENT_MANAGEMENT::adjust_migration_granularity(const START_ADDRESS_WIDTH start_address, const START_ADDRESS_WIDTH end_address, MIGRATION_GRANULARITY_WIDTH& migration_granularity)
{
    START_ADDRESS_WIDTH updated_end_address = end_address;

    // Check whether this migration granularity is beyond the block's range
    while (true)
    {
        if ((start_address + migration_granularity - 1) >= START_ADDRESS_WIDTH(StartAddress::Max))
        {
#if (FLEXIBLE_GRANULARITY == ENABLE)
            migration_granularity = end_address - start_address + 1;
#else
            if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_64) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_64);
            }
            else
            {
                std::cout << __func__ << ": migration granularity calculation error." << std::endl;
                assert(false);
            }
#endif // FLEXIBLE_GRANULARITY
        }
        else
        {
            // This migration granularity is within the block's range.
            updated_end_address = start_address + migration_granularity - 1;
            break;
        }
    }

    return updated_end_address;
}

START_ADDRESS_WIDTH OS_TRANSPARENT_MANAGEMENT::round_down_migration_granularity(const START_ADDRESS_WIDTH start_address, const START_ADDRESS_WIDTH end_address, MIGRATION_GRANULARITY_WIDTH& migration_granularity)
{
    START_ADDRESS_WIDTH updated_end_address = end_address;

    // Check whether this migration granularity is beyond the block's end address
    while (true)
    {
        if ((start_address + migration_granularity - 1) > end_address)
        {
#if (FLEXIBLE_GRANULARITY == ENABLE)
            migration_granularity = end_address - start_address + 1;
#else
            if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_4)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_2)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::KiB_1)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_512)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_256)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128);
            }
            else if ((MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_64) < migration_granularity) && (migration_granularity <= MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_128)))
            {
                migration_granularity = MIGRATION_GRANULARITY_WIDTH(MigrationGranularity::Byte_64);
            }
            else
            {
                std::cout << __func__ << ": migration granularity calculation error." << std::endl;
                assert(false);
            }
#endif // FLEXIBLE_GRANULARITY
        }
        else
        {
            // This migration granularity is within the block's end address.
            updated_end_address = start_address + migration_granularity - 1;
            break;
        }
    }

    return updated_end_address;
}

#endif // IDEAL_VARIABLE_GRANULARITY
#endif // MEMORY_USE_OS_TRANSPARENT_MANAGEMENT
