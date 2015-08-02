#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <locale.h>
#include <wchar.h>
#include <ctype.h>
#include <assert.h>
#include <stdarg.h>

#include "redo_log_reader.h"

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "en_US.utf8");
    const char* dummy = getenv("RLR_DBG");
    if (dummy) log_level = atoi(dummy);

    if (argc != 2) {
        show_usages();
        return 1;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 2;
    }

    parse_log_header();

    buf_t mtr_buffer = { .buffer_offset = 0, .buffer_len = 0 };
    buf_t* mtr_buf = &mtr_buffer;
    byte* buf_ptr;
    byte type;

    s_mtr_t mtr;
    while (1) {
        /* block contents: mtr */
        int flag = 1;
        while (flag) {
            clear_mtr(&mtr);

            print_log(1, "DEBUG file offset 0x%08llx buffer(%"PRIu64" / %lu,"
                    " file start +%llu buffer start +%llu)\n",
                    B2F(mtr_buf), mtr_buf->buffer_offset, mtr_buf->buffer_len,
                    mtr_buf->start_file_offset, mtr_buf->start_buffer_offset);
            buf_ptr = read_buffer_n(&mtr.type, mtr_buf, 1);
            if (!buf_ptr) goto done;

            type = mtr.type;
            if (mtr_is_single_rec(&mtr)) {
                type &= (byte)~MLOG_SINGLE_REC_FLAG;
                log_indent = 0;
            } else {
                log_indent = 1;
            }

            if (type != MLOG_MULTI_REC_END && type != MLOG_DUMMY_RECORD
                && type != MLOG_CHECKPOINT)
            {
                buf_ptr = read_compressed(&mtr.space_id, mtr_buf);
                if (!buf_ptr) goto done;

                buf_ptr = read_compressed(&mtr.page_no, mtr_buf);
                if (!buf_ptr) goto done;
            } else {
                log_indent = 0;
            }

            /* if (type != MLOG_1BYTE && type != MLOG_2BYTES */
            /*         && type != MLOG_4BYTES && type != MLOG_8BYTES) */
            show_mtr(&mtr);

            /* recv_parse_or_apply_log_rec_body */
            switch (type) {
                case MLOG_1BYTE:
                case MLOG_2BYTES:
                case MLOG_4BYTES:
                case MLOG_8BYTES: {
                    uint16_t page_offset;
                    buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
                    if (!buf_ptr) goto done;

                    print_log(0, "page offset: %"PRIu16" ", page_offset);
                    if (type == MLOG_8BYTES) {
                        uint64_t val;
                        buf_ptr = read_compressed_64(&val, mtr_buf);
                        if (!buf_ptr) goto done;
                        print_log(0, "value: %"PRIu64"\n", val);
                    } else {
                        uint32_t val;
                        buf_ptr = read_compressed(&val, mtr_buf);
                        if (!buf_ptr) goto done;
                        print_log(0, "value: %"PRIu32"\n", val);
                    }
                    break;
                }
                case MLOG_REC_INSERT:
                case MLOG_COMP_REC_INSERT: {
                    buf_ptr = parse_index(type == MLOG_COMP_REC_INSERT, mtr_buf);
                    if (!buf_ptr) goto done;

                    ssize_t bytes = parse_insert_rec(0, mtr_buf);
                    if (bytes == 0) goto done;

                    break;
                }
                case MLOG_LIST_END_COPY_CREATED:
                case MLOG_COMP_LIST_END_COPY_CREATED: {
                    buf_ptr = parse_index(type == MLOG_COMP_LIST_END_COPY_CREATED, mtr_buf);
                    if (!buf_ptr) goto done;

                    uint32_t data_len;
                    buf_ptr = read_buffer_n(&data_len, mtr_buf, 4);
                    if (!buf_ptr) goto done;

                    print_log(0, "data_len: %"PRIu32"\n", data_len);
                    ssize_t bytes_count;
                    while (data_len > 0) {
                        bytes_count = parse_insert_rec(1, mtr_buf);
                        assert(bytes_count > 0 && bytes_count <= data_len);
                        data_len -= bytes_count;
                    }
                    break;
                }
                case MLOG_LIST_END_DELETE:
                case MLOG_COMP_LIST_END_DELETE:
                case MLOG_LIST_START_DELETE:
                case MLOG_COMP_LIST_START_DELETE: {
                    buf_ptr = parse_index(
                            type == MLOG_COMP_LIST_END_DELETE
                            || type == MLOG_COMP_LIST_START_DELETE,
                            mtr_buf
                    );
                    if (!buf_ptr) goto done;

                    uint16_t page_offset;
                    buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
                    if (!buf_ptr) goto done;
                    print_log(0, "page offset: %"PRIu16"\n", page_offset);
                    break;
                }
                case MLOG_PAGE_REORGANIZE:
                case MLOG_COMP_PAGE_REORGANIZE: {
                    buf_ptr = parse_index(type == MLOG_COMP_PAGE_REORGANIZE, mtr_buf);
                    if (!buf_ptr) goto done;
                    break;
                }
                case MLOG_UNDO_INSERT: {
                    uint16_t len;
                    buf_ptr = read_buffer_n(&len, mtr_buf, 2);
                    if (!buf_ptr) goto done;
                    print_log(0, "length: %"PRIu16"\n", len);

                    buf_ptr = read_buffer_n(NULL, mtr_buf, len);
                    if (!buf_ptr) goto done;
                    mtr_buf->buffer_offset += len;
                    hexdump(buf_ptr, len);
                    break;
                }
                case MLOG_REC_UPDATE_IN_PLACE:
                case MLOG_COMP_REC_UPDATE_IN_PLACE: {
                    buf_ptr = parse_index(type == MLOG_COMP_REC_UPDATE_IN_PLACE, mtr_buf);
                    if (!buf_ptr) goto done;
                    uint8_t flags;
                    buf_ptr = read_buffer_n(&flags, mtr_buf, 1);
                    if (!buf_ptr) goto done;
                    print_log(0, "flags: %"PRIu8"\n", flags);

                    uint32_t pos;
                    buf_ptr = read_compressed(&pos, mtr_buf);
                    if (!buf_ptr) goto done;

                    uint64_t roll_ptr, trx_id;
                    buf_ptr = read_buffer_n(&roll_ptr, mtr_buf, DATA_ROLL_PTR_LEN);
                    if (!buf_ptr) goto done;
                    buf_ptr = read_compressed_64(&trx_id, mtr_buf);
                    if (!buf_ptr) goto done;

                    print_log(0, "TRX_ID position in record: %"PRIx32", roll ptr: 0x%"PRIx64"\n"
                           "TRX_ID: 0x%016"PRIx64"\n",
                           pos, roll_ptr, trx_id);

                    uint16_t page_offset;
                    buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
                    if (!buf_ptr) goto done;
                    print_log(0, "page offset: %"PRIu16"\n", page_offset);

                    uint8_t info_bits;
                    buf_ptr = read_buffer_n(&info_bits, mtr_buf, 1);
                    if (!buf_ptr) goto done;

                    uint32_t n_fields;
                    buf_ptr = read_compressed(&n_fields, mtr_buf);
                    if (!buf_ptr) goto done;

                    print_log(0, "info_bits: %"PRIu8", n_fields: %"PRIu32"\n",
                            info_bits, n_fields);
                    uint32_t i, field_no, len, delta;
                    for (i=0; i<n_fields; ++i) {
                        buf_ptr = read_compressed(&field_no, mtr_buf);
                        if (!buf_ptr) goto done;
                        buf_ptr = read_compressed(&len, mtr_buf);
                        if (!buf_ptr) goto done;
                        print_log(0, "field_no: %"PRIu32", len: %"PRIu32"\n", field_no, len);
                        while (len > 0) {
                            buf_ptr = read_buffer_n(NULL, mtr_buf, len);
                            if (!buf_ptr) goto done;
                            delta = mtr_buf->buffer_len < len ?
                                    mtr_buf->buffer_len : len;
                            mtr_buf->buffer_offset += delta;

                            assert(delta > 0);
                            hexdump(buf_ptr, delta);
                            len -= delta;
                        }
                    }

                    break;
                }
                case MLOG_REC_DELETE:
                case MLOG_COMP_REC_DELETE: {
                    buf_ptr = parse_index(type == MLOG_COMP_REC_DELETE, mtr_buf);
                    if (!buf_ptr) goto done;

                    uint16_t page_offset;
                    buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
                    if (!buf_ptr) goto done;
                    print_log(0, "page offset: %"PRIu16"\n", page_offset);
                    break;
                }
                case MLOG_REC_SEC_DELETE_MARK: {
                    uint8_t val;
                    buf_ptr = read_buffer_n(&val, mtr_buf, 1);
                    if (!buf_ptr) goto done;

                    uint16_t page_offset;
                    buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
                    if (!buf_ptr) goto done;

                    print_log(0, "val: %"PRIu8", page offset: %"PRIu16"\n", val, page_offset);
                    break;
                }
                case MLOG_REC_CLUST_DELETE_MARK:
                case MLOG_COMP_REC_CLUST_DELETE_MARK: {
                    buf_ptr = parse_index(type == MLOG_COMP_REC_CLUST_DELETE_MARK, mtr_buf);
                    if (!buf_ptr) goto done;
                    uint8_t flags, val;

                    buf_ptr = read_buffer_n(&flags, mtr_buf, 1);
                    if (!buf_ptr) goto done;

                    buf_ptr = read_buffer_n(&val, mtr_buf, 1);
                    if (!buf_ptr) goto done;

                    print_log(0, "flags: %"PRIu8", val: %"PRIu8"\n", flags, val);

                    uint32_t pos;
                    buf_ptr = read_compressed(&pos, mtr_buf);
                    if (!buf_ptr) goto done;

                    uint64_t roll_ptr, trx_id;
                    buf_ptr = read_buffer_n(&roll_ptr, mtr_buf, DATA_ROLL_PTR_LEN);
                    if (!buf_ptr) goto done;
                    buf_ptr = read_compressed_64(&trx_id, mtr_buf);
                    if (!buf_ptr) goto done;

                    print_log(0, "TRX_ID position in record: %"PRIx32", roll ptr: 0x%"PRIx64"\n"
                           "TRX_ID: 0x%016"PRIx64"\n",
                           pos, roll_ptr, trx_id);

                    uint16_t page_offset;
                    buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
                    if (!buf_ptr) goto done;
                    print_log(0, "page offset: %"PRIu16"\n", page_offset);
                    break;
                }
                case MLOG_WRITE_STRING: {
                    uint16_t page_offset, len;
                    buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
                    if (!buf_ptr) goto done;
                    buf_ptr = read_buffer_n(&len, mtr_buf, 2);
                    if (!buf_ptr) goto done;
                    print_log(0, "page offset: %"PRIu16", len: %"PRIu16"\n",
                            page_offset, len);

                    buf_ptr = read_buffer_n(NULL, mtr_buf, len);
                    if (!buf_ptr) goto done;
                    mtr_buf->buffer_offset += len;
                    hexdump(buf_ptr, len);
                    break;
                }
                case MLOG_UNDO_INIT: {
                    uint32_t seg_type;
                    buf_ptr = read_compressed(&seg_type, mtr_buf);
                    if (!buf_ptr) goto done;
                    print_log(0, "undo log segment type: %"PRIu32"\n", seg_type);
                    break;
                }
                case MLOG_UNDO_HDR_CREATE:
                case MLOG_UNDO_HDR_REUSE: {
                    uint64_t trx_id;
                    buf_ptr = read_compressed_64(&trx_id, mtr_buf);
                    print_log(0, "TRX_ID: %"PRIu64"\n", trx_id);
                    break;
                }
                case MLOG_FILE_CREATE:
                case MLOG_FILE_DELETE: {
                    uint16_t name_len;
                    buf_ptr = read_buffer_n(&name_len, mtr_buf, 2);
                    if (!buf_ptr) goto done;

                    buf_ptr = read_buffer_n(NULL, mtr_buf, name_len);
                    if (!buf_ptr) goto done;
                    mtr_buf->buffer_offset += name_len;
                    print_log(0, "filename: %s\n", buf_ptr);
                    break;
                }
                case MLOG_FILE_RENAME: {
                    uint16_t name_len;
                    buf_ptr = read_buffer_n(&name_len, mtr_buf, 2);
                    if (!buf_ptr) goto done;

                    buf_ptr = read_buffer_n(NULL, mtr_buf, name_len);
                    if (!buf_ptr) goto done;
                    mtr_buf->buffer_offset += name_len;
                    print_log(0, "old filename: %s\n", buf_ptr);

                    buf_ptr = read_buffer_n(&name_len, mtr_buf, 2);
                    if (!buf_ptr) goto done;

                    buf_ptr = read_buffer_n(NULL, mtr_buf, name_len);
                    if (!buf_ptr) goto done;
                    mtr_buf->buffer_offset += name_len;
                    print_log(0, "new filename: %s\n", buf_ptr);

                    break;
                }
                case MLOG_INIT_FILE_PAGE:
                case MLOG_IBUF_BITMAP_INIT:
                case MLOG_PAGE_CREATE:
                case MLOG_COMP_PAGE_CREATE:
                case MLOG_MULTI_REC_END: break;
                default:
                   print_log(0, "[WARNING] This MTR cannot be parsed (not yet implemented). "
                          "mtr type number: %"PRIu8", "
                          "buffer_offset %"PRIu64", "
                          "buffer_length %lu\n",
                          type, mtr_buf->buffer_offset, mtr_buf->buffer_len);
                   flag = 0;
                   break;
            }
            /* end of mtr */
        }
    }
done:
    log_indent = 0;
    print_log(0, "done");
    return 0;
}

uint8_t mtr_is_single_rec(const s_mtr_t* mtr) {
    return !!(mtr->type & MLOG_SINGLE_REC_FLAG);
}

const char* mtr_type_name(const s_mtr_t* mtr) {
    byte type = mtr->type;
    if (mtr_is_single_rec(mtr)) {
        type &= (byte)~MLOG_SINGLE_REC_FLAG;
    }
    switch (type) {
        case MLOG_1BYTE:
            return "MLOG_1BYTE";
        case MLOG_2BYTES:
            return "MLOG_2BYTES";
        case MLOG_4BYTES:
            return "MLOG_4BYTES";
        case MLOG_8BYTES:
            return "MLOG_8BYTES";
        case MLOG_REC_INSERT:
            return "MLOG_REC_INSERT";
        case MLOG_REC_CLUST_DELETE_MARK:
            return "MLOG_REC_CLUST_DELETE_MARK";
        case MLOG_REC_SEC_DELETE_MARK:
            return "MLOG_REC_SEC_DELETE_MARK";
        case MLOG_REC_UPDATE_IN_PLACE:
            return "MLOG_REC_UPDATE_IN_PLACE";
        case MLOG_REC_DELETE:
            return "MLOG_REC_DELETE";
        case MLOG_LIST_END_DELETE:
            return "MLOG_LIST_END_DELETE";
        case MLOG_LIST_START_DELETE:
            return "MLOG_LIST_START_DELETE";
        case MLOG_LIST_END_COPY_CREATED:
            return "MLOG_LIST_END_COPY_CREATED";
        case MLOG_PAGE_REORGANIZE:
            return "MLOG_PAGE_REORGANIZE";
        case MLOG_PAGE_CREATE:
            return "MLOG_PAGE_CREATE";
        case MLOG_UNDO_INSERT:
            return "MLOG_UNDO_INSERT";
        case MLOG_UNDO_ERASE_END:
            return "MLOG_UNDO_ERASE_END";
        case MLOG_UNDO_INIT:
            return "MLOG_UNDO_INIT";
        case MLOG_UNDO_HDR_DISCARD:
            return "MLOG_UNDO_HDR_DISCARD";
        case MLOG_UNDO_HDR_REUSE:
            return "MLOG_UNDO_HDR_REUSE";
        case MLOG_UNDO_HDR_CREATE:
            return "MLOG_UNDO_HDR_CREATE";
        case MLOG_REC_MIN_MARK:
            return "MLOG_REC_MIN_MARK";
        case MLOG_IBUF_BITMAP_INIT:
            return "MLOG_IBUF_BITMAP_INIT";
        case MLOG_LSN:
            return "MLOG_LSN";
        case MLOG_INIT_FILE_PAGE:
            return "MLOG_INIT_FILE_PAGE";
        case MLOG_WRITE_STRING:
            return "MLOG_WRITE_STRING";
        case MLOG_MULTI_REC_END:
            return "MLOG_MULTI_REC_END";
        case MLOG_DUMMY_RECORD:
            return "MLOG_DUMMY_RECORD";
        case MLOG_FILE_CREATE:
            return "MLOG_FILE_CREATE";
        case MLOG_FILE_RENAME:
            return "MLOG_FILE_RENAME";
        case MLOG_FILE_DELETE:
            return "MLOG_FILE_DELETE";
        case MLOG_COMP_REC_MIN_MARK:
            return "MLOG_COMP_REC_MIN_MARK";
        case MLOG_COMP_PAGE_CREATE:
            return "MLOG_COMP_PAGE_CREATE";
        case MLOG_COMP_REC_INSERT:
            return "MLOG_COMP_REC_INSERT";
        case MLOG_COMP_REC_CLUST_DELETE_MARK:
            return "MLOG_COMP_REC_CLUST_DELETE_MARK";
        case MLOG_COMP_REC_SEC_DELETE_MARK:
            return "MLOG_COMP_REC_SEC_DELETE_MARK";
        case MLOG_COMP_REC_UPDATE_IN_PLACE:
            return "MLOG_COMP_REC_UPDATE_IN_PLACE";
        case MLOG_COMP_REC_DELETE:
            return "MLOG_COMP_REC_DELETE";
        case MLOG_COMP_LIST_END_DELETE:
            return "MLOG_COMP_LIST_END_DELETE";
        case MLOG_COMP_LIST_START_DELETE:
            return "MLOG_COMP_LIST_START_DELETE";
        case MLOG_COMP_LIST_END_COPY_CREATED:
            return "MLOG_COMP_LIST_END_COPY_CREATED";
        case MLOG_COMP_PAGE_REORGANIZE:
            return "MLOG_COMP_PAGE_REORGANIZE";
        case MLOG_FILE_CREATE2:
            return "MLOG_FILE_CREATE2";
        case MLOG_ZIP_WRITE_NODE_PTR:
            return "MLOG_ZIP_WRITE_NODE_PTR";
        case MLOG_ZIP_WRITE_BLOB_PTR:
            return "MLOG_ZIP_WRITE_BLOB_PTR";
        case MLOG_ZIP_WRITE_HEADER:
            return "MLOG_ZIP_WRITE_HEADER";
        case MLOG_ZIP_PAGE_COMPRESS:
            return "MLOG_ZIP_PAGE_COMPRESS";
        case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
            return "MLOG_ZIP_PAGE_COMPRESS_NO_DATA";
        case MLOG_ZIP_PAGE_REORGANIZE:
            return "MLOG_ZIP_PAGE_REORGANIZE";
        default:
            return "UNKNOW";
    }
}

void clear_mtr(s_mtr_t *mtr) {
    mtr->type = mtr->space_id = mtr->page_no = 0;
}

byte* read_buffer_n(void* dst, buf_t* mtr_buf, const ssize_t n) {
    assert(!dst || (dst && n <= 9));
    if (mtr_buf->buffer_offset + n > mtr_buf->buffer_len) {
        ssize_t remain = mtr_buf->buffer_len - mtr_buf->buffer_offset;
        if (remain >= 0) {
            memcpy(mtr_buf->buffer,
                   mtr_buf->buffer + mtr_buf->buffer_offset,
                   remain
                  );

            mtr_buf->buffer_offset = remain;
            mtr_buf->start_buffer_offset = remain;
            mtr_buf->start_file_offset = file_offset;
            read_block_into_buffer(mtr_buf);
            mtr_buf->buffer_offset = 0;
        } else {
            mtr_buf->buffer_offset = mtr_buf->buffer_len = 0;
        }
    }
    if (mtr_buf->buffer_len == 0) return NULL;

    byte* val_ptr = mtr_buf->buffer + mtr_buf->buffer_offset;
    if (dst) {
        if (n == 1) {
            READ_N(*(uint8_t*)dst, mtr_buf->buffer + mtr_buf->buffer_offset, n);
        } else if (n == 2) {
            READ_N(*(uint16_t*)dst, mtr_buf->buffer + mtr_buf->buffer_offset, n);
        } else if (n > 2 && n <= 4) {
            READ_N(*(uint32_t*)dst, mtr_buf->buffer + mtr_buf->buffer_offset, n);
        } else {
            READ_N(*(uint64_t*)dst, mtr_buf->buffer + mtr_buf->buffer_offset, n);
        }
        mtr_buf->buffer_offset += n;
    }

    /* return pointer to the value */
    return val_ptr;
}

void read_block_into_buffer(buf_t* mtr_buf) {
    byte block_buffer[OS_FILE_LOG_BLOCK_SIZE];
    block_hdr block_header;

    ssize_t ret, incr_len;
    mtr_buf->buffer_len = mtr_buf->buffer_offset;
    while (mtr_buf->buffer_len <= MEMORY_BUFFER_SIZE - OS_FILE_LOG_BLOCK_SIZE) {
        ret = pread(fd, block_buffer,
                OS_FILE_LOG_BLOCK_SIZE, file_offset);
        if (ret != OS_FILE_LOG_BLOCK_SIZE) break;

        file_offset += OS_FILE_LOG_BLOCK_SIZE;
        parse_block_header(block_buffer, &block_header);

        if (block_header.block_data_len == 0) break;
        if (mtr_buf->buffer_offset == 0 && block_header.first_rec_group != 0)
            mtr_buf->buffer_offset = mtr_buf->buffer_len +
                block_header.first_rec_group - LOG_BLOCK_HDR_SIZE;

        incr_len = block_header.block_data_len - LOG_BLOCK_HDR_SIZE;
        if (block_header.block_data_len >= OS_FILE_LOG_BLOCK_SIZE)
            incr_len -= LOG_BLOCK_TRL_SIZE;
        memcpy(
                mtr_buf->buffer + mtr_buf->buffer_len,
                block_buffer + LOG_BLOCK_HDR_SIZE,
                incr_len
              );
        mtr_buf->buffer_len += incr_len;
    }
}

byte* read_compressed(uint32_t* dst, buf_t* mtr_buf) {
    byte* buf = read_buffer_n(NULL, mtr_buf, 5);
    if (!buf) return NULL;

    uint8_t flag = (uint8_t)(*buf & 0xFFUL);
    if (flag < 0x80UL) {
        mtr_buf->buffer_offset += 1;
        *dst = *buf;
    } else if (flag < 0xC0UL) {
        READ_N(*dst, buf, 2);
        mtr_buf->buffer_offset += 2;
        *dst &= 0x7FFFUL;
    } else if (flag < 0xE0UL) {
        READ_N(*dst, buf, 3);
        mtr_buf->buffer_offset += 3;
        *dst &= 0x3FFFFFUL;
    } else if (flag < 0xF0UL) {
        READ_N(*dst, buf, 4);
        mtr_buf->buffer_offset += 4;
        *dst &= 0x1FFFFFFFUL;
    } else {
        READ_N(*dst, buf+1, 4);
        mtr_buf->buffer_offset += 5;
    }
    return buf;
}

byte* read_compressed_64(uint64_t* val, buf_t* mtr_buf) {
    uint32_t half = 0;

    byte* buf = read_buffer_n(NULL, mtr_buf, 9);
    if (!buf) return NULL;

    byte* buf2 = read_compressed((uint32_t *)val, mtr_buf);
    if (!buf2) return NULL;
    *val <<= 32;

    buf2 = read_buffer_n(&half, mtr_buf, 4);
    if (!buf2) return NULL;
    *val |= half;

    return buf;
}

void parse_log_header() {
    byte log_hdr_buf[LOG_FILE_HDR_SIZE];
    long ret = pread(fd, &log_hdr_buf, LOG_FILE_HDR_SIZE, 0);
    assert(ret == LOG_FILE_HDR_SIZE);
    file_offset = LOG_FILE_HDR_SIZE;

    log_hdr log_header;
    READ(log_header.log_group_id, log_hdr_buf + LOG_GROUP_ID);
    READ(log_header.start_lsn, log_hdr_buf    + LOG_FILE_START_LSN);
    READ(log_header.log_file_no, log_hdr_buf  + LOG_FILE_NO);
    READ(log_header.arch_completed, log_hdr_buf + LOG_FILE_ARCH_COMPLETED);
    READ(log_header.end_lsn, log_hdr_buf        + LOG_FILE_END_LSN);
    READ(log_header.checkpoint1, log_hdr_buf    + LOG_CHECKPOINT_1);
    READ(log_header.checkpoint2, log_hdr_buf    + LOG_CHECKPOINT_2);

    show_log_header(&log_header);
}

void parse_block_header(const byte* buffer, block_hdr* block_header) {
    READ(block_header->block_no, buffer + LOG_BLOCK_HDR_NO);
    block_header->flush_bit =
        (LOG_BLOCK_FLUSH_BIT_MASK & block_header->block_no) ? 1 : 0;
    block_header->block_no =
        ~LOG_BLOCK_FLUSH_BIT_MASK & block_header->block_no;
    READ(block_header->block_data_len, buffer + LOG_BLOCK_HDR_DATA_LEN);
    READ(block_header->first_rec_group,
            buffer + LOG_BLOCK_FIRST_REC_GROUP);
    READ(block_header->check_point_no, buffer + LOG_BLOCK_CHECKPOINT_NO);

#if 0
    if (block_header.block_data_len > 0) show_block_header(block_header);
#endif
}

byte* parse_index(const uint8_t comp, buf_t* mtr_buf) {
    uint16_t idx_num, uniq_idx_num, i, column_len;
    byte* buf_ptr = mtr_buf->buffer + mtr_buf->buffer_offset;
    if (comp) {
        buf_ptr = read_buffer_n(&idx_num, mtr_buf, 2);
        if (!buf_ptr) return NULL;

        buf_ptr = read_buffer_n(&uniq_idx_num, mtr_buf, 2);
        if (!buf_ptr) return NULL;
    } else {
        idx_num = uniq_idx_num = 1;
    }
    print_log(0, "number of columns in index: %"PRIu16"\n"
           "number of columns in unique index: %"PRIu16"\n",
           idx_num, uniq_idx_num);
    byte *ret_ptr = buf_ptr;
    if (comp) {
        for (i=0; i<idx_num; ++i) {
            buf_ptr = read_buffer_n(&column_len, mtr_buf, 2);
            if (!buf_ptr) return NULL;
            /* The high-order bit of len is the NOT NULL flag;
             * the rest is 0 or 0x7fff for variable-length fields,
             * 1..0x7ffe for fixed-length fields. */
            print_log(0, "%"PRIu16" column in index", i);
            print_log(0, " > nullable: %s",
                    column_len & 0x8000 ? "no" : "yes");
            print_log(0, " > fixed/variable len: %s\n",
                    ((column_len + 1) & 0x7fff) <= 1 ?
                    "variable" : "fixed");
        }
    }
    return ret_ptr;
}

ssize_t parse_insert_rec(const uint8_t is_short, buf_t* mtr_buf) {
    byte* buf_ptr;
    off_t saved_offset;
    ssize_t bytes_count = 0;

    uint16_t page_offset;
    if (!is_short) {
        buf_ptr = read_buffer_n(&page_offset, mtr_buf, 2);
        if (!buf_ptr) return 0;
        bytes_count += 2;
        print_log(0, "page offset: %"PRIu16"\n", page_offset);
    }

    saved_offset = mtr_buf->buffer_offset;
    uint32_t end_seg_len;
    buf_ptr = read_compressed(&end_seg_len, mtr_buf);
    if (!buf_ptr) return bytes_count;
    bytes_count += (mtr_buf->buffer_offset - saved_offset);

    if (end_seg_len & 0x1UL) {
        uint8_t info_and_status_bits;
        buf_ptr = read_buffer_n(&info_and_status_bits, mtr_buf, 1);
        if (!buf_ptr) return bytes_count;
        bytes_count += 1;

        saved_offset = mtr_buf->buffer_offset;
        uint32_t origin_offset;
        buf_ptr = read_compressed(&origin_offset, mtr_buf);
        if (!buf_ptr) return bytes_count;
        bytes_count += (mtr_buf->buffer_offset - saved_offset);

        saved_offset = mtr_buf->buffer_offset;
        uint32_t mismatch_index;
        buf_ptr = read_compressed(&mismatch_index, mtr_buf);
        if (!buf_ptr) return bytes_count;
        bytes_count += (mtr_buf->buffer_offset - saved_offset);

        print_log(0, "origin  offset: %"PRIu32"\n"
               "mismatch index: %"PRIu32"\n",
               origin_offset, mismatch_index);
    }
    end_seg_len >>= 1;
    print_log(0, "end seg len: %"PRIu32"\n", end_seg_len);

    bytes_count += end_seg_len;
    /* buf_ptr = read_buffer_n(buffer, buffer_offset, buffer_len, end_seg_len); */
    /* buffer_offset += end_seg_len; */
    /* hexdump(buf_ptr, end_seg_len); */
    uint32_t delta;
    while (end_seg_len > 0) {
        buf_ptr = read_buffer_n(NULL, mtr_buf, end_seg_len);
        if (!buf_ptr) return bytes_count - end_seg_len;
        delta = mtr_buf->buffer_len - mtr_buf->buffer_offset < end_seg_len ?
                mtr_buf->buffer_len - mtr_buf->buffer_offset : end_seg_len;
        assert(delta > 0);
        hexdump(buf_ptr, delta);
        end_seg_len -= delta;
        mtr_buf->buffer_offset += delta;
    }

    return bytes_count;
}

void show_usages(void) {
    print_log(0, "Usages: redo-log-reader /path/to/ib_logfile\n");
}

void show_log_header(const log_hdr* log_header) {
    print_log(0, "============ LOG FILE HEADER ==============\n");
    print_log(0, "log group id : %"PRIu32"\n", log_header->log_group_id);
    print_log(0, "start lsn    : %"PRIu64"\n", log_header->start_lsn);
    print_log(0, "end lsn      : %"PRIu64"\n", log_header->end_lsn);
    print_log(0, "log file no  : %"PRIu32"\n", log_header->log_file_no);
    print_log(0, "checkpoint1  : %"PRIu64"\n", log_header->checkpoint1);
    print_log(0, "checkpoint2  : %"PRIu64"\n", log_header->checkpoint2);
}

void show_block_header(const block_hdr* block_header) {
    print_log(0, "============== BLOCK HEADER ================\n");
    print_log(0, "block no        : %"PRIu32"\n", block_header->block_no);
    print_log(0, "block_data_len  : %"PRIu16"\n", block_header->block_data_len);
    print_log(0, "first_rec_group : %"PRIu16"\n", block_header->first_rec_group);
    print_log(0, "check_point_no  : %"PRIu32"\n", block_header->check_point_no);
    print_log(0, "flush_bit       : %"PRIu8"\n" , block_header->flush_bit);
}

void show_mtr(const s_mtr_t* mtr) {
    print_log(0, "MTR: type(%s, %s) space_id(%"PRIu32") page_no(%"PRIu32")\n",
            mtr_type_name(mtr),
            mtr_is_single_rec(mtr) ? "single" : "multi",
            mtr->space_id, mtr->page_no);
}

void hexdump(const byte* ptr, ssize_t len) {
    ssize_t k, i, j = HEXDUMP_COLUMN_LEN;
    char str[HEXDUMP_COLUMN_LEN + 1];
    uint8_t snip = len > 256 ? 1 : 0;
    if (len > 256) len = 256;
#if 0
    wchar_t w_str[HEXDUMP_COLUMN_LEN + 1];
#endif
    for (i=0; i<j; ++i) {
        printf("%02x%s", i<len ? 0xFF & ((char*)ptr)[i] : '\0', i&1?" ":"");
        if ((i+1)%HEXDUMP_COLUMN_LEN == 0) {
            memset(str, 0, HEXDUMP_COLUMN_LEN + 1);
            memcpy(
              str, ptr + (i/HEXDUMP_COLUMN_LEN) * HEXDUMP_COLUMN_LEN,
              i < len ? HEXDUMP_COLUMN_LEN : len + HEXDUMP_COLUMN_LEN - j
            );
            if (j < len) j += HEXDUMP_COLUMN_LEN;
#if 0
            swprintf(w_str, sizeof(w_str)/sizeof(*w_str), L"%s", str);
            wprintf(L": %ls\n", w_str);
#endif
            k = 0;
            while (k < HEXDUMP_COLUMN_LEN) {
                putchar(isprint(str[k]) ? str[k] : '.');
                ++k;
            }
            printf("\n");
        }
    }
    if (snip) printf("... snip ...\n");
}

void print_log(const int level, const char *format, ...) {
    if (level > log_level) return;
    int i;
    for (i=0;i<log_indent;++i) printf(" ");
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
