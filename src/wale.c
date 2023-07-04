#include<wale.h>

#include<wale_get_lock_util.h>
#include<storage_byte_ordering.h>
#include<random_read_util.h>
#include<append_only_write_util.h>
#include<master_record_io.h>

#include<cutlery_stds.h>
#include<cutlery_math.h>

#include<stdlib.h>

static void random_readers_prefix(wale* wale_p)
{
	// take lock if the lock is internal
	if(wale_p->has_internal_lock)
		pthread_mutex_lock(get_wale_lock(wale_p));

	// wait if some writable task wants us to wait
	while(wale_p->waiting_for_random_readers_to_exit_flag)
	{
		wale_p->random_readers_waiting_count++;
		pthread_cond_wait(&(wale_p->random_readers_waiting), get_wale_lock(wale_p));
		wale_p->random_readers_waiting_count--;
	}

	// increment the random_readers_count notifying that there is someone reading now
	wale_p->random_readers_count++;

	// we perform reads from the wale file and the on_disk_master_record without holding the global lock
	pthread_mutex_unlock(get_wale_lock(wale_p));
}

static void random_readers_suffix(wale* wale_p)
{
	pthread_mutex_lock(get_wale_lock(wale_p));

	// decrement the random_readers_count
	wale_p->random_readers_count--;

	// if the random_readers_count has reached 0, due to us decrementing it, and there is someone waiting for all the readers to exit, then notify them, i.e. wake them up
	if(wale_p->random_readers_count == 0 && wale_p->waiting_for_random_readers_to_exit_flag)
		pthread_cond_broadcast(&(wale_p->waiting_for_random_readers_to_exit));

	// release lock if the lock is internal
	if(wale_p->has_internal_lock)
		pthread_mutex_unlock(get_wale_lock(wale_p));
}

/*
	Every reader function must read only the fluh contents of the WALe file,
	i.e. after calling random_readers_prefix() they can access only the on_disk_master_record and the file contents for the log_sequence_numbers between (and inclusive of) first_log_sequence_number and last_flushed_log_sequence_number
	and then call random_readers_suffix() before quiting

	on_disk_master_record is just the cached structured copy of the master record on disk
*/

uint64_t get_first_log_sequence_number(wale* wale_p)
{
	random_readers_prefix(wale_p);

	uint64_t first_log_sequence_number = wale_p->on_disk_master_record.first_log_sequence_number;

	random_readers_suffix(wale_p);

	return first_log_sequence_number;
}

uint64_t get_last_flushed_log_sequence_number(wale* wale_p)
{
	random_readers_prefix(wale_p);

	uint64_t last_flushed_log_sequence_number = wale_p->on_disk_master_record.last_flushed_log_sequence_number;

	random_readers_suffix(wale_p);

	return last_flushed_log_sequence_number;
}

uint64_t get_check_point_log_sequence_number(wale* wale_p)
{
	random_readers_prefix(wale_p);

	uint64_t check_point_log_sequence_number = wale_p->on_disk_master_record.check_point_log_sequence_number;

	random_readers_suffix(wale_p);

	return check_point_log_sequence_number;
}

uint64_t get_next_log_sequence_number_of(wale* wale_p, uint64_t log_sequence_number)
{
	random_readers_prefix(wale_p);

	// set it to INVALID_LOG_SEQUENCE_NUMBER, which is default result
	uint64_t next_log_sequence_number = INVALID_LOG_SEQUENCE_NUMBER;

	// if the wale has any records, and its first <= log_sequence_number < last, then read the size and add it to the log_sequence_number to get its next
	if(wale_p->on_disk_master_record.first_log_sequence_number != INVALID_LOG_SEQUENCE_NUMBER &&
		wale_p->on_disk_master_record.first_log_sequence_number <= log_sequence_number && 
		log_sequence_number < wale_p->on_disk_master_record.last_flushed_log_sequence_number
		)
	{
		uint64_t file_offset_of_log_record = log_sequence_number - wale_p->on_disk_master_record.first_log_sequence_number + wale_p->block_io_functions.block_size;

		char size_of_log_record_bytes[4];
		if(!random_read_at(size_of_log_record_bytes, 4, file_offset_of_log_record, &(wale_p->block_io_functions)))
			goto FAILED;

		uint32_t size_of_log_record = deserialize_le_uint32(size_of_log_record_bytes);

		next_log_sequence_number = log_sequence_number + size_of_log_record + 8; // 8 for prefix and suffix size

		FAILED:;
	}

	random_readers_suffix(wale_p);

	return next_log_sequence_number;
}

uint64_t get_prev_log_sequence_number_of(wale* wale_p, uint64_t log_sequence_number)
{
	random_readers_prefix(wale_p);

	// set it to INVALID_LOG_SEQUENCE_NUMBER, which is default result
	uint64_t prev_log_sequence_number = INVALID_LOG_SEQUENCE_NUMBER;

	// if the wale has any records, and its first < log_sequence_number <= last, then read the size of the previous log record and substract it from the log_sequence_number to get its prev
	if(wale_p->on_disk_master_record.first_log_sequence_number != INVALID_LOG_SEQUENCE_NUMBER &&
		wale_p->on_disk_master_record.first_log_sequence_number < log_sequence_number && 
		log_sequence_number <= wale_p->on_disk_master_record.last_flushed_log_sequence_number
		)
	{
		uint64_t file_offset_of_log_record = log_sequence_number - wale_p->on_disk_master_record.first_log_sequence_number + wale_p->block_io_functions.block_size;

		char size_of_prev_log_record_bytes[4];
		if(!random_read_at(size_of_prev_log_record_bytes, 4, file_offset_of_log_record - 4, &(wale_p->block_io_functions)))
			goto FAILED;

		uint32_t size_of_prev_log_record = deserialize_le_uint32(size_of_prev_log_record_bytes);

		prev_log_sequence_number = log_sequence_number - size_of_prev_log_record - 8; // 8 for prefix and suffix size of the previous log record

		FAILED:;
	}

	random_readers_suffix(wale_p);

	return prev_log_sequence_number;
}

void* get_log_record_at(wale* wale_p, uint64_t log_sequence_number, uint32_t* log_record_size)
{
	random_readers_prefix(wale_p);

	// set it to NULL, which is default result
	void* log_record = NULL;

	// if the wale has any records, and its first <= log_sequence_number <= last
	if(wale_p->on_disk_master_record.first_log_sequence_number != INVALID_LOG_SEQUENCE_NUMBER &&
		wale_p->on_disk_master_record.first_log_sequence_number <= log_sequence_number && 
		log_sequence_number <= wale_p->on_disk_master_record.last_flushed_log_sequence_number
		)
	{
		uint64_t file_offset_of_log_record = log_sequence_number - wale_p->on_disk_master_record.first_log_sequence_number + wale_p->block_io_functions.block_size;

		char size_of_log_record_bytes[4];
		if(!random_read_at(size_of_log_record_bytes, 4, file_offset_of_log_record, &(wale_p->block_io_functions)))
			goto FAILED;

		(*log_record_size) = deserialize_le_uint32(size_of_log_record_bytes);

		log_record = malloc((*log_record_size));
		if(log_record == NULL)
			goto FAILED;

		if(!random_read_at(log_record, (*log_record_size), file_offset_of_log_record + 4, &(wale_p->block_io_functions)))
		{
			free(log_record);
			log_record = NULL;
			goto FAILED;
		}

		FAILED:;
	}

	random_readers_suffix(wale_p);

	return log_record;
}

static uint64_t get_file_offset_for_next_log_sequence_number_to_append(wale* wale_p)
{
	if(wale_p->in_memory_master_record.first_log_sequence_number == INVALID_LOG_SEQUENCE_NUMBER)
		return wale_p->block_io_functions.block_size;
	else
		return wale_p->in_memory_master_record.next_log_sequence_number - wale_p->in_memory_master_record.first_log_sequence_number + wale_p->block_io_functions.block_size;
}

static int is_file_offset_within_append_only_buffer(wale* wale_p, uint64_t file_offset)
{
	if(wale_p->buffer_start_block_id * wale_p->block_io_functions.block_size <= file_offset && file_offset < (wale_p->buffer_start_block_id + wale_p->buffer_block_count) * wale_p->block_io_functions.block_size)
		return 1;
	return 0;
}

static uint64_t get_log_sequence_number_for_next_log_record_and_advance_master_record(wale* wale_p, uint32_t log_record_size, int is_check_point)
{
	// compute the total slot size required by this new log record
	uint64_t total_log_record_slot_size = ((uint64_t)log_record_size) + 8; // 4 bytes for prefix size and 4 bytes for suffix size

	// its log sequence number will simply be the next log sequence number
	uint64_t log_sequence_number = wale_p->in_memory_master_record.next_log_sequence_number;

	// check for overflow of the next_log_sequence_number, upon alloting this slot
	uint64_t new_next_log_sequence_number = log_sequence_number + total_log_record_slot_size;
	if(new_next_log_sequence_number < log_sequence_number || new_next_log_sequence_number < total_log_record_slot_size)
		return INVALID_LOG_SEQUENCE_NUMBER;

	// if earlier there were no log records on the disk, then this will be the new first_log_sequence_number
	if(wale_p->in_memory_master_record.first_log_sequence_number == INVALID_LOG_SEQUENCE_NUMBER)
		wale_p->in_memory_master_record.first_log_sequence_number = log_sequence_number;

	// this will also be the new last_flushed_log_sequence_number
	wale_p->in_memory_master_record.last_flushed_log_sequence_number = log_sequence_number;

	// update check point if this is going to be a check_point log record
	if(is_check_point)
		wale_p->in_memory_master_record.check_point_log_sequence_number = log_sequence_number;

	// set the next_log_sequence_number to the new next_log_sequence_number
	wale_p->in_memory_master_record.next_log_sequence_number = new_next_log_sequence_number;

	return log_sequence_number;
}

// returns the number of bytes written,
// it will return lesser than data_size, only if a scroll fails in which case you must exit the application, since we can't receover from this
// append_slot is advanced by this function, suggesting a write
static uint64_t append_log_record_data(wale* wale_p, uint64_t* append_slot, const char* data, uint64_t data_size, uint64_t* total_bytes_to_write_for_this_log_record)
{
	uint64_t bytes_written = 0;
	while(bytes_written < data_size)
	{
		uint64_t bytes_to_write = min(wale_p->buffer_block_count * wale_p->block_io_functions.block_size - (*append_slot), data_size - bytes_written);
		memory_move(wale_p->buffer + (*append_slot), data + bytes_written, bytes_to_write);
		bytes_written += bytes_to_write;
		(*append_slot) += bytes_to_write;
		(*total_bytes_to_write_for_this_log_record) -= bytes_to_write;

		// if append slot is at the end of the wale's append only buffer, then attempt to scroll
		if((*append_slot) == wale_p->buffer_block_count * wale_p->block_io_functions.block_size)
		{
			// scrolling needs global lock
			pthread_mutex_lock(get_wale_lock(wale_p));

			wale_p->scrolling_in_progress = 1;

			while(wale_p->append_only_writers_count > 1)
				pthread_cond_wait(&(wale_p->waiting_for_append_only_writers_to_exit), get_wale_lock(wale_p));

			int scroll_success = scroll_append_only_buffer(wale_p);

			wale_p->scrolling_in_progress = 0;

			if(scroll_success)
			{
				// take the probably the first slot and advance the append_offset to how much we can write at most
				(*append_slot) = wale_p->append_offset;
				wale_p->append_offset = min(wale_p->append_offset + (*total_bytes_to_write_for_this_log_record), wale_p->buffer_block_count * wale_p->block_io_functions.block_size);

				// if there is space in the append_only_buffer then we wake other writers up
				if(wale_p->append_offset < wale_p->buffer_block_count * wale_p->block_io_functions.block_size)
					pthread_cond_broadcast(&(wale_p->append_only_writers_waiting));
			}

			pthread_mutex_unlock(get_wale_lock(wale_p));

			// if scroll was a failure we break out of the loop
			if(!scroll_success)
				break;
		}
	}

	return bytes_written;
}

uint64_t append_log_record(wale* wale_p, const void* log_record, uint32_t log_record_size, int is_check_point)
{
	// return value defaults to an INVALID_LOG_SEQUENCE_NUMBER
	uint64_t log_sequence_number = INVALID_LOG_SEQUENCE_NUMBER;

	if(wale_p->has_internal_lock)
		pthread_mutex_lock(get_wale_lock(wale_p));

	if(wale_p->major_scroll_error)
		goto EXIT;

	// we wait while some writer thread wants us to wait OR if the offset for the next_log_sequence_number is not within the append only buffer
	while(wale_p->waiting_for_append_only_writers_to_exit_flag || wale_p->scrolling_in_progress ||
		is_file_offset_within_append_only_buffer(wale_p, get_file_offset_for_next_log_sequence_number_to_append(wale_p)))
	{
		wale_p->append_only_writers_waiting_count++;
		pthread_cond_wait(&(wale_p->append_only_writers_waiting), get_wale_lock(wale_p));
		wale_p->append_only_writers_waiting_count--;
	}

	// take slot if the next log sequence number is in the append only buffer
	log_sequence_number = get_log_sequence_number_for_next_log_record_and_advance_master_record(wale_p, log_record_size, is_check_point);

	// exit suggesting failure to allocate a log_sequence_number
	if(log_sequence_number == INVALID_LOG_SEQUENCE_NUMBER)
		goto EXIT;

	// compute the total bytes we will write
	uint64_t total_bytes_to_write = ((uint64_t)log_record_size) + 8;

	// now take the slot in the append only buffer
	uint64_t append_slot = wale_p->append_offset;

	// advance the append_offset of the append only buffer
	wale_p->append_offset = min(wale_p->append_offset + total_bytes_to_write, wale_p->buffer_block_count * wale_p->block_io_functions.block_size);

	// now we are truely a writer, since we have a slot and a log_sequence_number
	wale_p->append_only_writers_count++;

	// we have the slot in the append only buffer, and a log_sequence_number, now we don't need the global lock
	pthread_mutex_unlock(get_wale_lock(wale_p));

	// serialize log_record_size as a byte array ordered in little endian format
	char size_in_bytes[4];
	serialize_le_uint32(size_in_bytes, log_record_size);

	// write prefix
	uint64_t bytes_written = append_log_record_data(wale_p, &append_slot, size_in_bytes, 4, &total_bytes_to_write);
	if(bytes_written < 4)
		goto SCROLL_FAIL;

	// write log record itself
	bytes_written = append_log_record_data(wale_p, &append_slot, log_record, log_record_size, &total_bytes_to_write);
	if(bytes_written < log_record_size)
		goto SCROLL_FAIL;

	// write suffix
	bytes_written = append_log_record_data(wale_p, &append_slot, size_in_bytes, 4, &total_bytes_to_write);
	if(bytes_written < 4)
		goto SCROLL_FAIL;

	SCROLL_FAIL:;
	pthread_mutex_lock(get_wale_lock(wale_p));

	// this condition implies a fail to scroll the append only buffer
	if(total_bytes_to_write > 0)
	{
		log_sequence_number = INVALID_LOG_SEQUENCE_NUMBER;
		wale_p->major_scroll_error = 1;
	}

	// decrement the append only writers count, we are no longer appending
	wale_p->append_only_writers_count--;

	// wake up any one waiting for append only writers to exit
	if((wale_p->append_only_writers_count == 0 && wale_p->waiting_for_append_only_writers_to_exit_flag) ||
		(wale_p->append_only_writers_count == 1 && wale_p->scrolling_in_progress))
		pthread_cond_broadcast(&(wale_p->waiting_for_append_only_writers_to_exit));

	EXIT:;

	if(wale_p->has_internal_lock)
		pthread_mutex_unlock(get_wale_lock(wale_p));

	return log_sequence_number;
}

uint64_t flush_all_log_records(wale* wale_p)
{
	if(wale_p->has_internal_lock)
		pthread_mutex_lock(get_wale_lock(wale_p));

	while(wale_p->flush_in_progress)
	{
		wale_p->flush_completion_waiting_count++;
		pthread_cond_wait(&(wale_p->flush_completion_waiting), get_wale_lock(wale_p));
		wale_p->flush_completion_waiting_count--;
	}

	wale_p->flush_in_progress = 1;

	wale_p->flush_waiting_for_append_only_writers_to_exit = 1;

	while(wale_p->append_only_writers_count > 0)
		pthread_cond_wait(&(wale_p->waiting_for_append_only_writers_to_exit), get_wale_lock(wale_p));

	scroll_append_only_buffer(wale_p);

	wale_p->flush_waiting_for_append_only_writers_to_exit = 0;

	if(wale_p->append_only_writers_waiting_count > 0)
		pthread_cond_broadcast(&(wale_p->append_only_writers_waiting));

	pthread_mutex_unlock(get_wale_lock(wale_p));

	wale_p->block_io_functions.flush_all_writes(wale_p->block_io_functions.block_io_ops_handle);

	write_and_flush_master_record(&(wale_p->in_memory_master_record), &(wale_p->block_io_functions));

	pthread_mutex_lock(get_wale_lock(wale_p));

	wale_p->flush_waiting_for_random_readers_to_exit = 1;

	while(wale_p->random_readers_count > 0)
		pthread_cond_wait(&(wale_p->waiting_for_append_only_writers_to_exit), get_wale_lock(wale_p));

	pthread_mutex_unlock(get_wale_lock(wale_p));

	wale_p->on_disk_master_record = wale_p->in_memory_master_record;

	uint64_t last_flushed_log_sequence_number = wale_p->on_disk_master_record.last_flushed_log_sequence_number;

	pthread_mutex_lock(get_wale_lock(wale_p));

	wale_p->flush_waiting_for_random_readers_to_exit = 0;

	if(wale_p->random_readers_waiting_count > 0)
		pthread_cond_broadcast(&(wale_p->random_readers_waiting));

	wale_p->flush_in_progress = 0;

	if(wale_p->flush_completion_waiting_count)
		pthread_cond_broadcast(&(wale_p->flush_completion_waiting));

	if(wale_p->has_internal_lock)
		pthread_mutex_unlock(get_wale_lock(wale_p));

	return last_flushed_log_sequence_number;
}

int truncate_log_records(wale* wale_p);