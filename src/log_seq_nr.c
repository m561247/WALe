#include<log_seq_nr.h>

#include<cutlery_math.h>

int compare_log_seg_nr(log_seq_nr a, log_seq_nr b)
{
	int res = 0;
	for(uint32_t i = LOG_SEQ_NR_LIMBS_COUNT; i > 0 && res == 0;)
	{
		i--;
		res = compare_numbers(a.limbs[i], b.limbs[i]);
	}
	return res;
}

// carry_in must be 0 or 1 only
static limb_type add_log_seq_nr_overflow_unsafe_with_carry(log_seq_nr* res, log_seq_nr a, log_seq_nr b, limb_type carry)
{
	carry = !!carry;
	for(uint32_t i = 0; i < LOG_SEQ_NR_LIMBS_COUNT; i++)
	{
		limb_type carry_in = carry;
		res->limbs[i] = a.limbs[i] + b.limbs[i] + carry_in;
		carry = will_unsigned_sum_overflow(limb_type, a.limbs[i], b.limbs[i]) || will_unsigned_sum_overflow(limb_type, a.limbs[i] + b.limbs[i], carry_in);
	}
	return carry;
}

limb_type add_log_seq_nr_overflow_unsafe(log_seq_nr* res, log_seq_nr a, log_seq_nr b)
{
	return add_log_seq_nr_overflow_unsafe_with_carry(res, a, b, 0);
}

int add_log_seq_nr(log_seq_nr* res, log_seq_nr a, log_seq_nr b, log_seq_nr max_limit);

static log_seq_nr bitwise_not(log_seq_nr a)
{
	log_seq_nr res;
	for(uint32_t i = 0; i < LOG_SEQ_NR_LIMBS_COUNT; i++)
		res.limbs[i] = ~a.limbs[i];
	return res;
}

limb_type sub_log_seq_nr_underflow_unsafe(log_seq_nr* res, log_seq_nr a, log_seq_nr b)
{
	log_seq_nr not_b = bitwise_not(b);
	return add_log_seq_nr_overflow_unsafe_with_carry(res, a, not_b, 1);
}

int sub_log_seq_nr(log_seq_nr* res, log_seq_nr a, log_seq_nr b);

int set_bit_in_log_seq_nr(log_seq_nr* res, uint32_t bit_index);

void serialize_log_seq_nr(void* bytes, uint32_t bytes_size, log_seq_nr l);

log_seq_nr deserialize_le_uint64(const char* bytes, uint32_t bytes_size);

void print_log_seq_nr(log_seq_nr l);