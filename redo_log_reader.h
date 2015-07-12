#ifndef redo_log_reader_h
#define redo_log_reader_h

#include "mysql_simple.h"

#define MEMORY_BUFFER_SIZE (OS_FILE_LOG_BLOCK_SIZE*128)
#define HEXDUMP_COLUMN_LEN 16

#define BYTE_N(n) (8*(n))
#define READ_N(dst,src,n) { \
    dst=0; uint64_t i; \
    for (i=0; i<n; ++i) dst |= (((uint64_t)(*(src+i)))<<BYTE_N(n-i-1));\
}
#define READ(dst,src) READ_N(dst,src,sizeof(dst))

typedef struct log_hdr {
    /* first block */
    uint32_t log_group_id;
    uint64_t start_lsn;
    uint32_t log_file_no;
    /* second block */
    uint32_t arch_completed;
    uint64_t end_lsn;
    uint64_t checkpoint1;
    /* forth block */
    uint64_t checkpoint2;
} log_hdr;

typedef struct block_hdr {
    uint32_t block_no;
    uint16_t block_data_len;
    uint16_t first_rec_group;
    uint32_t check_point_no;
    uint8_t  flush_bit;
} block_hdr;

typedef struct simple_mtr_t {
    uint8_t  type;
    uint32_t space_id;
    uint32_t page_no;
} s_mtr_t;

typedef struct buffer_t {
    byte buffer[MEMORY_BUFFER_SIZE];
    off_t buffer_offset;
    off_t start_buffer_offset;
    off_t start_file_offset;
    ssize_t buffer_len;
} buf_t;

void hexdump(const byte*, ssize_t);

void read_block_into_buffer(buf_t *);
byte* read_buffer_n(void*, buf_t*, const ssize_t);
/* mach_read_compressed */
byte* read_compressed(uint32_t*, buf_t*);
byte* read_compressed_64(uint64_t*, buf_t*);

void parse_log_header();
void parse_block_header(const byte*, block_hdr*);
byte* parse_index(const uint8_t, buf_t*);
ssize_t parse_insert_rec(const uint8_t, buf_t*);

void clear_mtr(s_mtr_t *);
uint8_t mtr_is_single_rec(const s_mtr_t*);
const char* mtr_type_name(const s_mtr_t*);

void show_usages(void);
void show_log_header(const log_hdr*);
void show_block_header(const block_hdr*);
void show_mtr(const s_mtr_t*);

static int fd;
static off_t file_offset;

static int log_level = 0;
static int log_indent = 0;
void print_log(const int, const char*, ...);

#define B2F(mtr_buf) \
    (mtr_buf->start_file_offset \
     + (mtr_buf->buffer_offset - mtr_buf->start_buffer_offset) \
     / LOG_BLOCK_DATA_SIZE * OS_FILE_LOG_BLOCK_SIZE \
     + LOG_BLOCK_HDR_SIZE \
     + (mtr_buf->buffer_offset - mtr_buf->start_buffer_offset) \
     % LOG_BLOCK_DATA_SIZE \
    )

#endif
